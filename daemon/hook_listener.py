#!/usr/bin/env python3
"""Claude Code hook listener for the Clawdmeter daemon.

Claude Code can POST hook events to an HTTP endpoint (`"type": "http"` hooks),
which is how the device learns what each running Claude Code session is doing.
This module owns that endpoint and the in-memory session table it feeds.

Three deliberate design choices:

1. The table holds far MORE per session than the BLE payload can carry (~200
   usable bytes, 5 rows). Adding a field to the screen, or redesigning it
   entirely, should only touch `to_ble_payload()` at the bottom of this file --
   never the ingest path. Ingest records everything the hook gives us.

2. Platform-neutral. Claude Code performs the POST itself, so unlike the
   PowerShell notifier hooks there is nothing per-OS here, and both
   `claude_usage_daemon.py` (macOS) and `claude_usage_daemon_windows.py` import
   this unchanged. CONFIG_FILE differs between them, so it is passed in rather
   than hardcoded.

3. No new third-party dependency. The server is `asyncio.start_server` plus
   just enough HTTP/1.1 to read a Content-Length JSON body. Both daemons
   already run an asyncio loop, so this is one more task on it -- adding
   aiohttp would mean a new dependency on three platforms for one endpoint.

Security: binds loopback only and rejects non-loopback peers. Hook payloads
contain prompt text (`last_assistant_message`, `message_text`), so an
0.0.0.0 bind would leak session content to the LAN.
"""
from __future__ import annotations

import asyncio
import ipaddress
import json
import os
import re
import sys
import time
from collections import deque
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path

# --- Tunables -------------------------------------------------------------
# All deliberately far above what BLE sends, per the "don't rewrite later"
# requirement. Memory cost is trivial: a few KB per session.

DEFAULT_PORT = 25293          # "clawd" on a phone keypad
MAX_SESSIONS = 64             # vs 5 rows sent over BLE
MAX_TEXT = 2000               # retained chars of prompt/response text
MAX_EVENT_HISTORY = 32        # recent events kept per session, for debugging
MAX_BODY_BYTES = 1 << 20      # 1 MiB request cap
READ_TIMEOUT_SEC = 5.0
RETAIN_ENDED_SEC = 120        # keep ENDED sessions briefly so the screen can show them
TRANSCRIPT_TAIL_BYTES = 256 * 1024   # transcript lines can be tens of KB each

# --- Liveness -------------------------------------------------------------
# Backstop only. The roster below is the real liveness signal; this timer just
# stops a session lingering forever when the roster is unreadable. It used to be
# the ONLY signal at 6h, which was wrong in both directions: it evicted sessions
# left idle overnight while they were genuinely alive, and let crashed ones squat
# a row for hours showing a stale "idle".
STALE_AFTER_SEC = 6 * 3600

# ~/.claude/sessions/<pid>.json exists only while a Claude Code process is
# running, and carries the per-session name Claude Code derives itself
# ("netmap-57"), which is unique where basename(cwd) is not.
ROSTER_CACHE_SEC = 5          # re-scan at most this often; called per BLE push
# A session must be this old before absence from the roster counts against it,
# so a hook arriving before its roster file is visible isn't declared dead.
ROSTER_GRACE_SEC = 30

# Context-limit heuristic. The authoritative percentage is the statusline's
# `context_window.used_percentage`, which hook payloads do NOT include, so we
# infer the limit from the model id and also keep the raw token count -- a
# future screen can present tokens directly rather than trusting this.
CONTEXT_LIMIT_DEFAULT = 200_000
CONTEXT_LIMIT_1M = 1_000_000

# --- BLE wire format ------------------------------------------------------
# Session rows ride their OWN characteristic (SS_CHAR), not the usage write.
# The usage payload already reaches 121 bytes worst case, which would leave only
# ~79 bytes here -- about two rows. A separate characteristic gets a fresh
# ~200-byte budget while keeping one wire format (JSON) for both.
BLE_MAX_ROWS = 5

# Label length is the ASPIRATION, not a guarantee -- `to_ble_payload` shrinks it
# to fit the budget and only drops rows once it hits the floor.
#
# 20 chars covers every real project name observed (longest: "data-pipeline-svc",
# 17) and costs 216 bytes worst case for 5 rows. Measured ATT_MTU on Windows is
# 256 (253 usable), so 26 would also fit -- but that spends the safety margin on
# name lengths nobody has, and macOS/BlueZ are still unmeasured.
BLE_LABEL_CHARS = 20
BLE_LABEL_CHARS_MIN = 8         # below this, names stop being recognisable

# Fallback for when the negotiated MTU is unknown. Deliberately conservative:
# the only hard evidence is that the 121-byte usage payload works everywhere, so
# an unmeasured link gets the old budget, which self-shrinks labels to 14.
# Daemons override this from client.mtu_size via budget_from_mtu().
BLE_ROWS_BUDGET_BYTES = 190

# Held back from MTU-3 so a slightly-off backend reading cannot push a write over
# the real ceiling, where write-without-response drops it silently.
BLE_WRITE_MARGIN = 10


def budget_from_mtu(mtu: int | None) -> int:
    """Row budget for a measured ATT_MTU, or the conservative default.

    A write carries MTU-3 bytes (ATT opcode + handle); the margin absorbs
    backend reporting quirks. Readings at or below the 23-byte BLE default are
    rejected as bogus: the existing usage payload proves the real value is far
    higher, so a low reading means the backend is guessing, not that the link is.
    """
    if not mtu or mtu <= 23:
        return BLE_ROWS_BUDGET_BYTES
    return max(BLE_ROWS_BUDGET_BYTES, mtu - 3 - BLE_WRITE_MARGIN)


def ble_fragment_size(fragment: dict) -> int:
    """Encoded size of a BLE fragment, in bytes on the wire."""
    return len(json.dumps(fragment, separators=(",", ":")).encode())


def ble_label(name: str, chars: int, keep_suffix: bool = False) -> str:
    """Fit a session name to `chars`, marking truncation with ASCII dots.

    The dots are counted INSIDE the cap, so signalling truncation costs nothing
    on the wire.

    With `keep_suffix`, elides the MIDDLE so Claude Code's trailing discriminator
    survives: two sessions in one directory are "netmap-57" and "netmap-12", and
    tail-truncating "somelongprojectname-12" to "somelongprojectn..." would put
    the collision straight back. Becomes "somelongprojec...-12" instead.

    `keep_suffix` is a flag rather than a pattern match on the name because the
    discriminator is NOT always numeric -- "clawdmeter-2c" was observed on
    hardware, so an earlier `-\\d+$` test silently failed to protect it. Widening
    the pattern instead would misfire on ordinary names: "data-pipeline-svc" would
    match "-formula" and get needlessly middle-elided. The caller knows whether
    the label came from the roster, so it passes that in.

    ASCII "..." rather than U+2026: the ellipsis would encode as 3 UTF-8 bytes
    anyway (no saving), and the Styrene fonts driving these rows only cover ASCII
    32-126 -- `range_start = 32, range_length = 95` in font_styrene_*.c -- so it
    would render as a missing glyph. Only the mono fonts have U+2026, which is
    why the status line can use it.
    """
    if len(name) <= chars:
        return name
    if chars <= 3:
        return name[:chars]

    m = re.search(r"-[A-Za-z0-9]+$", name) if keep_suffix else None
    if m:
        suffix = m.group(0)
        head_budget = chars - len(suffix) - 3
        # Only worth it if a usable stem survives; otherwise the suffix is eating
        # the whole budget and a plain truncation reads better.
        if head_budget >= 3:
            return name[:head_budget] + "..." + suffix

    return name[:chars - 3] + "..."


class SessionState(str, Enum):
    """What a session is doing. str-valued so logs and JSON dumps stay readable.

    Ordered roughly by how much it wants your attention; `ATTENTION_STATES`
    below is what a future auto-promote screen would key on.
    """
    STARTING = "starting"
    IDLE = "idle"                            # finished a turn, awaiting your prompt
    THINKING = "thinking"                    # model is working
    RESPONDING = "responding"                # streaming text to the user
    RUNNING_TOOL = "running_tool"
    COMPACTING = "compacting"
    WAITING_PERMISSION = "waiting_permission"
    WAITING_QUESTION = "waiting_question"    # AskUserQuestion
    WAITING_INPUT = "waiting_input"          # MCP elicitation / agent_needs_input
    ERROR = "error"                          # turn died (rate limit, overload, ...)
    ENDED = "ended"


ATTENTION_STATES = frozenset({
    SessionState.WAITING_PERMISSION,
    SessionState.WAITING_QUESTION,
    SessionState.WAITING_INPUT,
    SessionState.ERROR,
})

# Firmware-facing numeric codes. Append only -- these go over the wire, so
# renumbering would desync any daemon/firmware pair of different vintages.
BLE_STATE_CODES: dict[SessionState, int] = {
    SessionState.STARTING: 0,
    SessionState.IDLE: 1,
    SessionState.THINKING: 2,
    SessionState.RESPONDING: 3,
    SessionState.RUNNING_TOOL: 4,
    SessionState.COMPACTING: 5,
    SessionState.WAITING_PERMISSION: 6,
    SessionState.WAITING_QUESTION: 7,
    SessionState.WAITING_INPUT: 8,
    SessionState.ERROR: 9,
    SessionState.ENDED: 10,
}

# Model family codes, likewise append-only.
BLE_MODEL_CODES = {"opus": 1, "sonnet": 2, "haiku": 3, "fable": 4}


def _now() -> float:
    return time.time()


def _clip(text: object, limit: int = MAX_TEXT) -> str:
    """Coerce to str and bound the length. Hook text fields are unbounded."""
    if text is None:
        return ""
    s = text if isinstance(text, str) else str(text)
    return s[:limit]


@dataclass
class SessionInfo:
    """Everything we know about one Claude Code session.

    Far richer than the BLE payload on purpose -- see the module docstring.
    Fields are grouped by which hook event populates them.
    """
    session_id: str

    # Identity / provenance
    cwd: str = ""
    project: str = ""                # basename(cwd); fallback display label
    derived_name: str = ""           # e.g. "netmap-57", from ~/.claude/sessions/<pid>.json
    pid: int = 0
    kind: str = ""                   # interactive | (headless/-p runs)
    title: str = ""                  # SessionStart.session_title (undocumented, best-effort)
    model: str = ""                  # SessionStart.model (undocumented, best-effort)
    version: str = ""
    source: str = ""                 # startup | resume | clear | compact | fork
    transcript_path: str = ""
    agent_id: str = ""               # set when the event came from a subagent
    agent_type: str = ""

    # Live state
    state: SessionState = SessionState.STARTING
    state_since: float = field(default_factory=_now)
    started_at: float = field(default_factory=_now)
    last_event_at: float = field(default_factory=_now)
    ended_at: float | None = None
    end_reason: str = ""

    # Harness context
    permission_mode: str = ""
    effort: str = ""
    prompt_id: str = ""

    # Context window
    context_tokens: int | None = None
    context_limit: int | None = None

    # What it is waiting on / last did
    pending_tool_name: str = ""
    pending_tool_use_id: str = ""
    last_notification_type: str = ""
    last_notification_message: str = ""
    last_prompt: str = ""
    last_assistant_message: str = ""
    error_type: str = ""
    error_message: str = ""

    # Counters, for a future "3 subagents running" indicator
    turns: int = 0
    tool_calls: int = 0
    compactions: int = 0
    last_compact_trigger: str = ""
    active_subagents: int = 0
    subagents_started: int = 0
    subagents_finished: int = 0
    tasks_created: int = 0
    tasks_completed: int = 0
    errors: int = 0

    # Bounded ring of recent (timestamp, event_name, detail) for debugging
    history: deque = field(default_factory=lambda: deque(maxlen=MAX_EVENT_HISTORY))

    # --- Derived -----------------------------------------------------------

    @property
    def context_pct(self) -> int | None:
        """0-100, or None when unknown. Heuristic limit -- see module docstring."""
        if self.context_tokens is None:
            return None
        limit = self.context_limit or CONTEXT_LIMIT_DEFAULT
        if limit <= 0:
            return None
        return max(0, min(100, round(self.context_tokens * 100 / limit)))

    @property
    def state_elapsed(self) -> float:
        return max(0.0, _now() - self.state_since)

    @property
    def needs_attention(self) -> bool:
        return self.state in ATTENTION_STATES

    @property
    def label(self) -> str:
        """Display name, best available first.

        `derived_name` wins because it is the only option that is UNIQUE: it comes
        from ~/.claude/sessions/<pid>.json and carries Claude Code's own numeric
        discriminator, so two sessions in one directory read as "netmap-57" and
        "netmap-12" rather than "netmap" twice. Falls back to basename(cwd) when
        the roster is unreadable, which collides but is better than nothing.
        """
        return (self.derived_name or self.project or self.title
                or self.session_id[:8])

    def set_state(self, state: SessionState) -> None:
        """Stamp `state_since` only on an actual transition, so 'waiting 4m'
        measures the wait rather than the time since the last repeated event."""
        if state != self.state:
            self.state = state
            self.state_since = _now()

    def as_dict(self) -> dict:
        """Full snapshot -- for logging, tests, and any future richer transport."""
        out = {
            k: v for k, v in self.__dict__.items()
            if k != "history"
        }
        out["state"] = self.state.value
        out["context_pct"] = self.context_pct
        out["state_elapsed"] = round(self.state_elapsed, 1)
        out["needs_attention"] = self.needs_attention
        out["label"] = self.label
        out["history"] = list(self.history)
        return out


# --- Transcript reading ---------------------------------------------------

def _tail_text(path: Path, max_bytes: int = TRANSCRIPT_TAIL_BYTES) -> str:
    """Read the last `max_bytes` of a file. Transcripts reach several MB, so
    never read one whole."""
    with path.open("rb") as fh:
        fh.seek(0, os.SEEK_END)
        size = fh.tell()
        fh.seek(max(0, size - max_bytes))
        return fh.read().decode("utf-8", errors="replace")


def read_context_usage(
    transcript_path: str | os.PathLike,
) -> tuple[int | None, int | None, str]:
    """Return (tokens_in_context, inferred_limit, model_id) from a transcript.

    The model id is returned as well as used, because hook payloads only carry it
    on SessionStart -- so a session already running when the hooks were installed
    never reports one, and its rows went out with model code 0 (unknown). The
    transcript has it on every assistant turn.

    Context usage is the newest main-thread assistant record's
    input_tokens + cache_read_input_tokens + cache_creation_input_tokens.
    `isSidechain` records are subagent turns and would inflate the figure.
    Returns (None, None) on any problem -- an unreadable transcript must never
    take the daemon down.
    """
    try:
        path = Path(transcript_path)
        if not path.is_file():
            return None, None, ""
        chunk = _tail_text(path)
    except OSError:
        return None, None, ""

    lines = chunk.splitlines()
    # The first line is probably a fragment from mid-record; drop it unless the
    # tail happened to cover the whole file.
    if len(lines) > 1 and not lines[0].lstrip().startswith("{"):
        lines = lines[1:]

    for line in reversed(lines):
        line = line.strip()
        if not line or '"assistant"' not in line:
            continue
        try:
            rec = json.loads(line)
        except (ValueError, TypeError):
            continue
        if rec.get("type") != "assistant" or rec.get("isSidechain"):
            continue
        msg = rec.get("message")
        if not isinstance(msg, dict):
            continue
        usage = msg.get("usage")
        if not isinstance(usage, dict):
            continue
        tokens = 0
        for key in ("input_tokens", "cache_read_input_tokens",
                    "cache_creation_input_tokens"):
            val = usage.get(key)
            if isinstance(val, (int, float)):
                tokens += int(val)
        if tokens <= 0:
            continue
        model = msg.get("model") or ""
        return tokens, infer_context_limit(model, tokens), model
    return None, None, ""


def infer_context_limit(model: str, tokens: int = 0) -> int:
    """Best-effort context limit from a model id and the observed token count.

    Heuristic, not authoritative: the real number is the statusline's
    `context_window.used_percentage`, which hooks do not provide. Callers keep the
    raw token count too, so a screen can show tokens instead of a percentage.

    `tokens` exists because the model id alone is NOT sufficient. Transcripts
    record `message.model` as e.g. "claude-opus-5" with the "[1m]" suffix
    stripped, so a 1M-context session is indistinguishable from a 200k one by
    name. Observed on hardware: a long 1M session reported 100% full.

    A session whose context already exceeds 200k demonstrably has a larger
    window, so the token count is treated as evidence about the limit. Crude, but
    strictly better than pinning every long 1M session at 100%.
    """
    if "1m" in (model or "").lower():
        return CONTEXT_LIMIT_1M
    if tokens > CONTEXT_LIMIT_DEFAULT:
        return CONTEXT_LIMIT_1M
    return CONTEXT_LIMIT_DEFAULT


# --- Session roster (~/.claude/sessions) ----------------------------------

def claude_config_dirs(extra: list[Path] | None = None) -> list[Path]:
    """Claude Code config dirs to look for a session roster in.

    Precedence matches the daemon's own `config_dirs` semantics documented in
    config.example: an explicit list REPLACES the default rather than adding to
    it, so someone pointing the daemon at ~/.claude-work does not silently also
    get ~/.claude. Falls back to CLAUDE_CONFIG_DIR (Claude Code's own override,
    which accepts a comma-separated list), then to ~/.claude.
    """
    explicit = [Path(d) for d in (extra or []) if d]
    if explicit:
        deduped: list[Path] = []
        for d in explicit:
            if d not in deduped:
                deduped.append(d)
        return deduped

    if env := os.environ.get("CLAUDE_CONFIG_DIR"):
        dirs = [Path(p.strip()).expanduser() for p in env.split(",") if p.strip()]
        if dirs:
            return dirs

    return [Path.home() / ".claude"]


def _pid_alive(pid: int) -> bool:
    """True if `pid` is a running process.

    Catches the hard-crash case the roster alone can't: Claude Code removes its
    session file on a clean exit, but a kill -9 leaves it behind.

    Deliberately NOT os.kill(pid, 0) on Windows -- Python's os.kill there does not
    implement the POSIX "signal 0 means probe" convention and would attempt to
    terminate the process. Uses OpenProcess instead.
    """
    if pid <= 0:
        return False
    try:
        if sys.platform == "win32":
            import ctypes
            PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
            STILL_ACTIVE = 259
            k32 = ctypes.windll.kernel32
            handle = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
            if not handle:
                return False
            try:
                code = ctypes.c_ulong()
                if k32.GetExitCodeProcess(handle, ctypes.byref(code)):
                    return code.value == STILL_ACTIVE
                return True
            finally:
                k32.CloseHandle(handle)
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True          # exists, just not ours
    except Exception:        # noqa: BLE001 - liveness must never raise
        return True          # unknown: assume alive rather than hide a session


def scan_live_sessions(dirs: list[Path]) -> dict[str, dict] | None:
    """Map sessionId -> roster entry for every currently-running session.

    Returns None when no roster directory could be read at all, which the caller
    must treat as "unknown" rather than "nothing running" -- otherwise a missing
    directory would wipe the display.
    """
    roster: dict[str, dict] = {}
    saw_any_dir = False

    for base in dirs:
        sessions_dir = base / "sessions"
        try:
            if not sessions_dir.is_dir():
                continue
            entries = list(sessions_dir.glob("*.json"))
        except OSError:
            continue
        saw_any_dir = True

        for path in entries:
            try:
                rec = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                continue
            if not isinstance(rec, dict):
                continue
            sid = rec.get("sessionId")
            if not isinstance(sid, str) or not sid:
                continue
            pid = rec.get("pid") if isinstance(rec.get("pid"), int) else 0
            if pid and not _pid_alive(pid):
                continue          # stale file from a hard crash
            roster[sid] = {
                "pid": pid,
                "name": rec.get("name") if isinstance(rec.get("name"), str) else "",
                "cwd": rec.get("cwd") if isinstance(rec.get("cwd"), str) else "",
                "kind": rec.get("kind") if isinstance(rec.get("kind"), str) else "",
                "version": rec.get("version") if isinstance(rec.get("version"), str) else "",
            }

    return roster if saw_any_dir else None


def model_family(model: str) -> str:
    """'claude-opus-5[1m]' -> 'opus'. Empty string when unrecognised."""
    low = (model or "").lower()
    for fam in BLE_MODEL_CODES:
        if fam in low:
            return fam
    return ""


# --- Session table --------------------------------------------------------

class SessionTable:
    """The live view of every Claude Code session that has posted a hook.

    Single-event-loop by design, so no locking: `ingest` is only ever called
    from the listener's own task, and readers call `snapshot`/`to_ble_payload`
    from the same loop.
    """

    def __init__(self, max_sessions: int = MAX_SESSIONS,
                 config_dirs: list[Path] | None = None) -> None:
        self._sessions: dict[str, SessionInfo] = {}
        self._max_sessions = max_sessions
        self.events_received = 0
        self.last_event_at: float | None = None
        # Roster: sessionId -> entry, or None when unreadable ("unknown", not
        # "nothing running"). Cached because active() runs on every BLE push.
        self._config_dirs = claude_config_dirs(config_dirs)
        self._roster: dict[str, dict] | None = None
        self._roster_at: float = 0.0

    # -- roster ---------------------------------------------------------

    def refresh_roster(self, force: bool = False) -> dict[str, dict] | None:
        if force or (_now() - self._roster_at) >= ROSTER_CACHE_SEC:
            self._roster = scan_live_sessions(self._config_dirs)
            self._roster_at = _now()
        return self._roster

    def _apply_roster(self) -> None:
        """Enrich sessions with their derived name, and retire dead ones.

        Absence from the roster is only acted on after ROSTER_GRACE_SEC, so a
        session whose first hook beat its roster file is not declared dead. When
        the roster is unreadable this does nothing at all and the staleness timer
        in prune() remains the only backstop.
        """
        roster = self.refresh_roster()
        if roster is None:
            return
        now = _now()
        for sid, s in self._sessions.items():
            entry = roster.get(sid)
            if entry is not None:
                if entry["name"]:
                    s.derived_name = entry["name"]
                if entry["pid"]:
                    s.pid = entry["pid"]
                if entry["kind"]:
                    s.kind = entry["kind"]
                if entry["cwd"] and not s.cwd:
                    s.cwd = entry["cwd"]
            elif (s.state is not SessionState.ENDED
                  and now - s.started_at > ROSTER_GRACE_SEC):
                # The process is gone and never sent SessionEnd -- a crash, a
                # kill, or a machine that went to sleep mid-session.
                s.end_reason = s.end_reason or "process gone"
                s.ended_at = s.ended_at or now
                s.set_state(SessionState.ENDED)

    # -- reads ----------------------------------------------------------

    def __len__(self) -> int:
        return len(self._sessions)

    def get(self, session_id: str) -> SessionInfo | None:
        return self._sessions.get(session_id)

    def active(self) -> list[SessionInfo]:
        """Live sessions, most attention-worthy first, then most recently active.

        This ordering is what decides which sessions survive BLE truncation, so
        a session that wants you is never the one dropped.
        """
        self.prune()
        live = [s for s in self._sessions.values() if s.state is not SessionState.ENDED]
        live.sort(key=lambda s: (not s.needs_attention, -s.last_event_at))
        return live

    def snapshot(self) -> dict:
        """Everything, fully expanded. Diagnostics and tests."""
        self.prune()
        return {
            "events_received": self.events_received,
            "last_event_at": self.last_event_at,
            "sessions": [s.as_dict() for s in self.active()],
        }

    # -- writes ---------------------------------------------------------

    def prune(self) -> None:
        self._apply_roster()
        now = _now()
        for sid, s in list(self._sessions.items()):
            if s.state is SessionState.ENDED:
                if s.ended_at and now - s.ended_at > RETAIN_ENDED_SEC:
                    del self._sessions[sid]
            elif now - s.last_event_at > STALE_AFTER_SEC:
                # Claude Code died without a SessionEnd (crash, kill -9).
                del self._sessions[sid]

        # Hard cap as a backstop. Evict the least recently active.
        if len(self._sessions) > self._max_sessions:
            for sid, _ in sorted(
                self._sessions.items(), key=lambda kv: kv[1].last_event_at
            )[: len(self._sessions) - self._max_sessions]:
                del self._sessions[sid]

    def ingest(self, payload: dict) -> SessionInfo | None:
        """Apply one hook payload. Returns the touched session, or None if the
        payload was unusable. Never raises on malformed input -- a bad hook body
        must not disturb the BLE loop."""
        if not isinstance(payload, dict):
            return None
        session_id = payload.get("session_id")
        if not isinstance(session_id, str) or not session_id:
            return None
        event = payload.get("hook_event_name")
        if not isinstance(event, str) or not event:
            return None

        self.events_received += 1
        self.last_event_at = _now()

        s = self._sessions.get(session_id)
        if s is None:
            s = SessionInfo(session_id=session_id)
            self._sessions[session_id] = s

        _apply_common(s, payload)
        _apply_event(s, event, payload)

        # Pick up the derived name as early as possible, so the very first render
        # of a new session already shows "netmap-57" rather than "netmap".
        if not s.derived_name:
            roster = self.refresh_roster(force=(s.turns == 0 and s.pid == 0))
            if roster and (entry := roster.get(session_id)):
                s.derived_name = entry["name"] or s.derived_name
                s.pid = entry["pid"] or s.pid
                s.kind = entry["kind"] or s.kind

        s.last_event_at = _now()
        s.history.append((round(s.last_event_at, 3), event,
                          _clip(payload.get("notification_type")
                                or payload.get("tool_name") or "", 64)))
        self.prune()
        return s

    # -- BLE boundary ---------------------------------------------------

    def to_ble_payload(self, max_rows: int = BLE_MAX_ROWS,
                       max_bytes: int = BLE_ROWS_BUDGET_BYTES,
                       label_chars: int = BLE_LABEL_CHARS) -> dict:
        """Compact payload fragment for the SS characteristic.

        THIS is the only place that knows the wire format. Everything above
        stays untouched when the screen changes.

        Positional rows keep JSON overhead down. Row layout:
            [label, state_code, context_pct, state_elapsed_s, model_code]
        with context_pct -1 meaning "unknown".

        `max_bytes` is the hard constraint; `max_rows` and `label_chars` are soft.
        A write-without-response that exceeds MTU is dropped silently, so fitting
        the budget matters more than any presentation preference.

        Fitting order is deliberate: SHRINK LABELS FIRST, drop rows last. A
        shortened name is still recognisable, whereas a dropped row hides a
        session entirely -- and hiding sessions is the one thing this screen must
        not do. Rows are dropped from the tail, and attention-worthy sessions
        sort first (see `active`), so the last thing dropped is never the session
        that wants you.

        `sn` always carries the TRUE running total, not the number of rows sent,
        so the device can render "X more running" after any truncation.

        Keys are `ss` (sessions) and `sn` (total running). Note `t`/`tf` are
        already taken by the clock fields and `c` by chime.
        """
        live = self.active()
        total = len(live)
        chosen = live[:max_rows]

        def build(sessions, chars):
            return [[
                # Only a roster-derived label carries a discriminator worth
                # protecting; a bare basename(cwd) tail-truncates normally.
                ble_label(s.label, chars, keep_suffix=bool(s.derived_name)),
                BLE_STATE_CODES.get(s.state, 0),
                s.context_pct if s.context_pct is not None else -1,
                min(65535, int(s.state_elapsed)),
                BLE_MODEL_CODES.get(model_family(s.model), 0),
            ] for s in sessions]

        # Longest label that fits with every row present.
        for chars in range(label_chars, BLE_LABEL_CHARS_MIN - 1, -1):
            rows = build(chosen, chars)
            if ble_fragment_size({"ss": rows, "sn": total}) <= max_bytes:
                return {"ss": rows, "sn": total}

        # Still over at the minimum label length: now start dropping rows.
        rows = build(chosen, BLE_LABEL_CHARS_MIN)
        while rows and ble_fragment_size({"ss": rows, "sn": total}) > max_bytes:
            rows.pop()
        return {"ss": rows, "sn": total}


def _apply_common(s: SessionInfo, p: dict) -> None:
    """Fields every hook event carries."""
    if cwd := p.get("cwd"):
        if isinstance(cwd, str):
            s.cwd = cwd
            # PurePath would pick the wrong flavour for a Windows path parsed on
            # POSIX, so split on both separators.
            s.project = cwd.replace("\\", "/").rstrip("/").rsplit("/", 1)[-1]
    for src, dst in (("transcript_path", "transcript_path"),
                     ("permission_mode", "permission_mode"),
                     ("prompt_id", "prompt_id"),
                     ("agent_id", "agent_id"),
                     ("agent_type", "agent_type"),
                     ("version", "version")):
        val = p.get(src)
        if isinstance(val, str) and val:
            setattr(s, dst, val)
    effort = p.get("effort")
    if isinstance(effort, dict) and isinstance(effort.get("level"), str):
        s.effort = effort["level"]
    elif isinstance(effort, str):
        s.effort = effort


def _refresh_context(s: SessionInfo, zero_if_empty: bool = False) -> None:
    """Re-read context usage from the transcript. Only called on events where it
    can have moved (turn end, compaction), since it costs a 256 KB tail read.

    `zero_if_empty` treats an unreadable/empty transcript as 0 tokens rather than
    "unknown". Correct only at SessionStart, where there genuinely has not been an
    assistant turn yet; elsewhere an unreadable transcript must stay unknown so
    the device hides the bar instead of claiming 0% used.

    Passed explicitly rather than inferred from `s.state`: callers set the state
    before calling this, so a state check here would never see STARTING.
    """
    if not s.transcript_path:
        return
    tokens, limit, model = read_context_usage(s.transcript_path)
    # Hooks only carry the model on SessionStart, so a session already running
    # when the hooks were installed has none and its rows went out with model
    # code 0. The transcript has it on every assistant turn.
    if model and not s.model:
        s.model = model
    if tokens is not None:
        s.context_tokens = tokens
        s.context_limit = limit
    elif zero_if_empty:
        s.context_tokens = 0
        s.context_limit = infer_context_limit(s.model)


def _apply_event(s: SessionInfo, event: str, p: dict) -> None:
    """Map one hook event onto the session state machine.

    Unknown events are recorded in history (by the caller) but otherwise
    ignored, so a future Claude Code release adding events cannot break us.
    """
    if event == "SessionStart":
        s.started_at = _now()
        s.ended_at = None
        s.end_reason = ""
        if isinstance(p.get("source"), str):
            s.source = p["source"]
        # session_title and model are present in the binary but undocumented;
        # treat as best-effort extras.
        if isinstance(p.get("session_title"), str):
            s.title = _clip(p["session_title"], 200)
        if isinstance(p.get("model"), str):
            s.model = p["model"]
        s.set_state(SessionState.IDLE)
        # A resumed session already has turns on disk; a fresh one genuinely has
        # 0 context rather than unknown.
        _refresh_context(s, zero_if_empty=(s.source != "resume"))

    elif event == "SessionEnd":
        s.end_reason = _clip(p.get("end_reason"), 64)
        s.ended_at = _now()
        s.set_state(SessionState.ENDED)

    elif event == "UserPromptSubmit":
        s.last_prompt = _clip(p.get("prompt") or p.get("user_prompt"))
        s.set_state(SessionState.THINKING)

    elif event == "UserPromptExpansion":
        s.set_state(SessionState.THINKING)

    elif event == "PreToolUse":
        tool = _clip(p.get("tool_name"), 64)
        s.pending_tool_name = tool
        s.pending_tool_use_id = _clip(p.get("tool_use_id"), 64)
        s.tool_calls += 1
        # AskUserQuestion blocks on the user, so it is a wait, not work.
        s.set_state(SessionState.WAITING_QUESTION if tool == "AskUserQuestion"
                    else SessionState.RUNNING_TOOL)

    elif event in ("PostToolUse", "PostToolUseFailure", "PostToolBatch"):
        s.pending_tool_name = ""
        s.pending_tool_use_id = ""
        if s.state in (SessionState.RUNNING_TOOL, SessionState.WAITING_PERMISSION,
                       SessionState.WAITING_QUESTION):
            s.set_state(SessionState.THINKING)

    elif event == "PermissionRequest":
        s.pending_tool_name = _clip(p.get("tool_name"), 64)
        s.pending_tool_use_id = _clip(p.get("tool_use_id"), 64)
        s.set_state(SessionState.WAITING_PERMISSION)

    elif event == "PermissionDenied":
        if s.state is SessionState.WAITING_PERMISSION:
            s.set_state(SessionState.THINKING)

    elif event == "Notification":
        ntype = _clip(p.get("notification_type"), 64)
        s.last_notification_type = ntype
        s.last_notification_message = _clip(p.get("message"))
        mapped = {
            "permission_prompt": SessionState.WAITING_PERMISSION,
            "idle_prompt": SessionState.IDLE,
            "agent_needs_input": SessionState.WAITING_INPUT,
            "agent_completed": SessionState.IDLE,
            "elicitation_dialog": SessionState.WAITING_INPUT,
            "elicitation_complete": SessionState.THINKING,
            "elicitation_response": SessionState.THINKING,
        }.get(ntype)
        if mapped is not None:
            s.set_state(mapped)

    elif event == "MessageDisplay":
        s.set_state(SessionState.RESPONDING)

    elif event == "Stop":
        s.turns += 1
        s.last_assistant_message = _clip(p.get("last_assistant_message"))
        s.pending_tool_name = ""
        s.set_state(SessionState.IDLE)
        _refresh_context(s)

    elif event == "StopFailure":
        s.errors += 1
        s.error_type = _clip(p.get("error_type"), 64)
        s.error_message = _clip(p.get("error_message"))
        s.set_state(SessionState.ERROR)

    elif event == "PreCompact":
        s.set_state(SessionState.COMPACTING)

    elif event == "PostCompact":
        s.compactions += 1
        s.last_compact_trigger = _clip(p.get("trigger"), 32)
        s.set_state(SessionState.IDLE)
        _refresh_context(s)

    elif event in ("Elicitation",):
        s.set_state(SessionState.WAITING_INPUT)

    elif event in ("ElicitationResult",):
        s.set_state(SessionState.THINKING)

    elif event == "SubagentStart":
        s.subagents_started += 1
        s.active_subagents += 1

    elif event == "SubagentStop":
        s.subagents_finished += 1
        s.active_subagents = max(0, s.active_subagents - 1)

    elif event == "TaskCreated":
        s.tasks_created += 1

    elif event == "TaskCompleted":
        s.tasks_completed += 1


# --- Config ---------------------------------------------------------------

def read_hook_port(config_file: Path) -> int | None:
    """Read `hook_port` from the daemon config. None = listener disabled.

    Config path differs per platform, so the caller passes its own CONFIG_FILE
    (see the two daemons' module-level constants).

    Accepts `off`/`no`/`false`/`0` to disable and a port number to enable.
    Defaults to disabled, matching `chime = off` and `clock = off`: an upgrade
    should never silently open a listening socket.
    """
    raw = _read_config_value(config_file, "hook_port")
    if raw is None:
        return None
    low = raw.strip().lower()
    if low in ("", "off", "no", "false", "0", "disabled"):
        return None
    if low in ("on", "yes", "true", "default"):
        return DEFAULT_PORT
    try:
        port = int(low, 10)
    except ValueError:
        return None
    return port if 1 <= port <= 65535 else None


def read_roster_dirs(config_file: Path) -> list[Path] | None:
    """Parse `config_dirs` from the daemon config, for the session roster.

    Same option the usage poller already uses to watch more than one Claude
    config dir, so the two stay consistent: if you poll ~/.claude-work for usage,
    its sessions show on the device too. None = unset, use the default.
    """
    raw = _read_config_value(config_file, "config_dirs")
    if not raw:
        return None
    dirs = [Path(p.strip()).expanduser() for p in raw.split(",") if p.strip()]
    return dirs or None


def _read_config_value(config_file: Path, key: str) -> str | None:
    """`key = value` lookup matching the format the other settings use
    (`#` comments, case-insensitive keys, re-read on every call)."""
    try:
        if not config_file.exists():
            return None
        for line in config_file.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if "=" not in line:
                continue
            k, v = line.split("=", 1)
            if k.strip().lower() == key:
                return v.strip()
    except OSError:
        pass
    return None


# --- HTTP listener --------------------------------------------------------

_HTTP_204 = b"HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"


def _http_response(status: str, body: bytes = b"",
                   content_type: str = "application/json") -> bytes:
    return (
        f"HTTP/1.1 {status}\r\n"
        f"Content-Type: {content_type}\r\n"
        f"Content-Length: {len(body)}\r\n"
        "Connection: close\r\n\r\n"
    ).encode() + body


class HookListener:
    """Serves the hook endpoint and owns a `SessionTable`.

    Usage from either daemon's main():

        listener = HookListener(read_hook_port(CONFIG_FILE), log=log)
        await listener.start()          # no-op when port is None
        ...
        payload.update(listener.table.to_ble_payload())
    """

    def __init__(self, port: int | None, host: str = "127.0.0.1",
                 log=None, table: SessionTable | None = None,
                 config_dirs: list[Path] | None = None) -> None:
        self.port = port
        self.host = host
        self.table = (table if table is not None
                      else SessionTable(config_dirs=config_dirs))
        self._log = log or (lambda msg: None)
        self._server: asyncio.AbstractServer | None = None
        # Set whenever an event arrives, so the daemon can push to the device
        # promptly instead of waiting out its 60s poll.
        self.updated = asyncio.Event()

    @property
    def running(self) -> bool:
        return self._server is not None

    @property
    def bound_port(self) -> int | None:
        """The port actually bound. Differs from `self.port` when 0 was passed to
        let the OS choose, which is how the tests avoid clashing with a real
        daemon on 25293."""
        if self._server is None or not self._server.sockets:
            return None
        return self._server.sockets[0].getsockname()[1]

    async def start(self) -> bool:
        """Bind and serve. Returns False when disabled or the bind failed --
        the listener is an enhancement, so a port clash must not stop the
        daemon from doing its existing job."""
        if self.port is None:
            self._log("Hook listener disabled (set hook_port in config to enable)")
            return False
        if self._server is not None:
            return True
        try:
            self._server = await asyncio.start_server(
                self._handle, self.host, self.port
            )
        except OSError as e:
            self._log(f"Hook listener could not bind {self.host}:{self.port}: {e}")
            self._server = None
            return False
        self._log(f"Hook listener on http://{self.host}:{self.port}")
        return True

    async def stop(self) -> None:
        if self._server is None:
            return
        self._server.close()
        try:
            await self._server.wait_closed()
        except (OSError, asyncio.CancelledError):
            pass
        self._server = None

    async def __aenter__(self) -> HookListener:
        await self.start()
        return self

    async def __aexit__(self, *_exc) -> None:
        await self.stop()

    async def _handle(self, reader: asyncio.StreamReader,
                      writer: asyncio.StreamWriter) -> None:
        try:
            peer = writer.get_extra_info("peername")
            if not _is_loopback(peer):
                # Defence in depth: we bind loopback, but never process a
                # payload from off-box even if that changes.
                self._log(f"Hook listener rejecting non-loopback peer {peer}")
                writer.write(_http_response("403 Forbidden"))
                await writer.drain()
                return

            try:
                body = await asyncio.wait_for(
                    _read_http_body(reader), READ_TIMEOUT_SEC
                )
            except asyncio.TimeoutError:
                writer.write(_http_response("408 Request Timeout"))
                await writer.drain()
                return
            except ValueError as e:
                writer.write(_http_response("400 Bad Request"))
                await writer.drain()
                self._log(f"Hook listener bad request: {e}")
                return

            if body is None:
                writer.write(_HTTP_204)
                await writer.drain()
                return

            try:
                payload = json.loads(body)
            except (ValueError, TypeError):
                writer.write(_http_response("400 Bad Request"))
                await writer.drain()
                return

            session = self.table.ingest(payload)
            if session is not None:
                self.updated.set()

            # 2xx with an empty body is "success, no context added" -- the right
            # answer for an observer hook. Returning JSON here would be fed back
            # to Claude as context.
            writer.write(_HTTP_204)
            await writer.drain()
        except (ConnectionError, OSError):
            pass
        finally:
            try:
                writer.close()
            except OSError:
                pass


def _is_loopback(peer) -> bool:
    if not peer:
        return False
    host = peer[0] if isinstance(peer, tuple) else peer
    try:
        return ipaddress.ip_address(str(host).split("%", 1)[0]).is_loopback
    except ValueError:
        return False


async def _read_http_body(reader: asyncio.StreamReader) -> bytes | None:
    """Minimal HTTP/1.1: request line, headers, then Content-Length bytes.

    Claude Code always sends Content-Length with a JSON body, so chunked
    transfer-encoding is deliberately unsupported -- it would be dead code.
    Returns None for a request with no body (e.g. a health-check GET).

    Raises ValueError for anything malformed, including a truncated request or
    oversized headers: `readuntil`/`readexactly` signal those with exception
    types (IncompleteReadError, LimitOverrunError) that are not OSError, so
    without this mapping they would surface as unhandled task exceptions.
    """
    try:
        header_blob = await reader.readuntil(b"\r\n\r\n")
    except asyncio.LimitOverrunError as e:
        raise ValueError("headers too large") from e
    except (asyncio.IncompleteReadError, EOFError) as e:
        raise ValueError("truncated request") from e
    lines = header_blob.decode("latin-1").split("\r\n")
    if not lines or not lines[0]:
        raise ValueError("empty request line")

    length = 0
    for line in lines[1:]:
        if not line:
            continue
        name, _, value = line.partition(":")
        if name.strip().lower() == "content-length":
            try:
                length = int(value.strip())
            except ValueError:
                raise ValueError("bad content-length") from None
    if length < 0 or length > MAX_BODY_BYTES:
        raise ValueError(f"body length {length} out of range")
    if length == 0:
        return None
    try:
        return await reader.readexactly(length)
    except (asyncio.IncompleteReadError, EOFError) as e:
        raise ValueError("body shorter than content-length") from e


# --- Manual smoke test ----------------------------------------------------

if __name__ == "__main__":
    import pprint

    async def _main() -> None:
        def _log(msg: str) -> None:
            print(f"[hook] {msg}")

        port = int(os.environ.get("CLAWD_HOOK_PORT", DEFAULT_PORT))
        async with HookListener(port, log=_log) as listener:
            if not listener.running:
                return
            print("POST hook payloads to it; Ctrl-C to stop.")
            while True:
                await asyncio.sleep(5)
                if listener.updated.is_set():
                    listener.updated.clear()
                    pprint.pprint(listener.table.snapshot())
                    print("BLE fragment:",
                          json.dumps(listener.table.to_ble_payload(),
                                     separators=(",", ":")))

    try:
        asyncio.run(_main())
    except KeyboardInterrupt:
        pass
