#!/usr/bin/env python3
"""Claude Usage Tracker Daemon (BLE) — macOS port of claude-usage-daemon.sh.

Polls Claude API rate-limit headers and writes a JSON payload to the
ESP32 "Clawdmeter" peripheral over a custom GATT service. Uses
bleak (CoreBluetooth backend on macOS).
"""

import asyncio
import calendar
import datetime
import getpass
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

import httpx
from bleak import BleakClient
from bleak.exc import BleakError

DEVICE_NAME = "Clawdmeter"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"

POLL_INTERVAL = 60
TICK = 5
RETRY_AFTER_FAILURE_S = 30   # a failed poll retries after this, not every TICK
RATE_LIMIT_MIN_S = 60        # floor for backing off after an HTTP 429
HEARTBEAT_S = 60             # replay the last payload (aged) this often while polls fail…
HEARTBEAT_MAX_AGE_S = 900    # …for at most this long, then let the device show "No data"
CONNECT_TIMEOUT = 20.0

# macOS: token lives in Keychain (service "Claude Code-credentials").
# Linux: token lives in ~/.claude/.credentials.json.
KEYCHAIN_SERVICE = "Claude Code-credentials"
DEFAULT_CONFIG_DIR = Path.home() / ".claude"
SAVED_ADDR_FILE = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"
CONFIG_FILE = Path.home() / ".config" / "claude-usage-monitor" / "config"

API_URL = "https://api.anthropic.com/v1/messages"
API_HEADERS_TEMPLATE = {
    "anthropic-version": "2023-06-01",
    "anthropic-beta": "oauth-2025-04-20",
    "Content-Type": "application/json",
    "User-Agent": "claude-code/2.1.5",
}
API_BODY = {
    "model": "claude-haiku-4-5-20251001",
    "max_tokens": 1,
    "messages": [{"role": "user", "content": "hi"}],
}


# After an HTTP 429 no poll is attempted before this time (time.time()).
_rate_limited_until = 0.0


def age_payload(payload: dict, secs: float) -> dict:
    """Copy of a usage payload aged by ``secs``: reset countdowns shrink and the
    clock advances, so a replay reads correctly. Mirrors the Linux daemon."""
    aged = dict(payload)
    mins = int(secs) // 60
    for k in ("sr", "wr"):
        if isinstance(aged.get(k), int) and aged[k] > 0:
            aged[k] = max(aged[k] - mins, 0)
    if isinstance(aged.get("t"), int) and aged["t"] > 0:
        aged["t"] += int(secs)
    return aged


class TokenExpired(Exception):
    """Raised by poll_api on a 401/403 — the access token is dead. The daemon never
    refreshes (pure free-ride: Claude Code owns refreshing), so the caller just
    signals "No data" to the device until the CLI re-seeds the token."""


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def _extract_access_token(blob: str) -> str | None:
    """Pull the accessToken out of a credentials blob.

    Claude Code stores credentials as a JSON object; the blob may also be
    nested ({"claudeAiOauth": {"accessToken": "..."}}). Fall back to a
    regex match so unexpected shapes still work, and finally treat the
    blob as a raw token if nothing else matches.
    """
    blob = blob.strip()
    if not blob:
        return None
    try:
        data = json.loads(blob)
    except json.JSONDecodeError:
        data = None
    if isinstance(data, dict):
        # direct: {"accessToken": "..."}
        tok = data.get("accessToken")
        if isinstance(tok, str) and tok.strip():
            return tok
        # nested: {"claudeAiOauth": {"accessToken": "..."}}
        for v in data.values():
            if isinstance(v, dict):
                tok = v.get("accessToken")
                if isinstance(tok, str) and tok.strip():
                    return tok
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        return m.group(1)
    # Raw token (no JSON wrapper) — must look plausible (sk-ant-... etc.)
    if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
        return blob
    return None


def _decode_keychain_blob(raw: str) -> str:
    """Transparently decode a hex-dumped Keychain secret back to text.

    ``security … -w`` prints the password as a continuous hex string whenever
    the stored bytes aren't cleanly printable (e.g. an embedded newline). A
    normal credentials blob is JSON, which is never valid hex (it contains
    '{', '"', …), so all-hex detection is unambiguous and safe.
    """
    s = raw.strip()
    if s and len(s) % 2 == 0 and re.fullmatch(r"[0-9a-fA-F]+", s):
        try:
            return bytes.fromhex(s).decode("utf-8")
        except (ValueError, UnicodeDecodeError):
            return raw
    return raw


def _read_token_keychain() -> str | None:
    """Read the OAuth access token from the macOS Keychain, or None.

    ``security … -w`` may hex-dump the stored secret (see _decode_keychain_blob),
    so decode before extracting the access token.
    """
    try:
        out = subprocess.run(
            [
                "security",
                "find-generic-password",
                "-s",
                KEYCHAIN_SERVICE,
                "-a",
                getpass.getuser(),
                "-w",
            ],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except subprocess.CalledProcessError as e:
        log(f"Keychain read failed (rc={e.returncode}): {e.stderr.strip()}")
        return None
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        log(f"Keychain access error: {e}")
        return None
    return _extract_access_token(_decode_keychain_blob(out.stdout))


def read_config_dirs() -> list[Path]:
    """Claude config dirs to poll, from the `config_dirs` option (comma list).

    Defaults to [~/.claude] so existing single-plan setups are unchanged. ~ is
    expanded. Mirrors the Linux bash daemon's read_config_dirs.
    """
    raw = ""
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "config_dirs":
                    raw = val.strip()
    except OSError:
        pass
    if not raw:
        return [DEFAULT_CONFIG_DIR]
    dirs = [Path(p.strip()).expanduser() for p in raw.split(",") if p.strip()]
    return dirs or [DEFAULT_CONFIG_DIR]


def read_token_for(config_dir: Path) -> str | None:
    """Read the OAuth token for one config dir.

    Linux: each dir keeps its own ``<dir>/.credentials.json``. macOS: the default
    install stores the token in Keychain with no file, so for the default dir we
    fall back to Keychain when no file is present — preserving existing
    single-plan macOS behavior. Additional macOS dirs are read from their files;
    a work plan whose token lives only in the single Keychain entry can't be told
    apart there (documented follow-up).
    """
    cred = config_dir / ".credentials.json"
    try:
        if cred.exists():
            return _extract_access_token(cred.read_text())
    except OSError as e:
        log(f"Error reading credentials in {config_dir}: {e}")
    if sys.platform == "darwin" and config_dir == DEFAULT_CONFIG_DIR:
        return _read_token_keychain()
    return None


def load_cached_address() -> str | None:
    if not SAVED_ADDR_FILE.exists():
        return None
    addr = SAVED_ADDR_FILE.read_text().strip()
    # Accept both Linux MAC (AA:BB:CC:DD:EE:FF) and macOS CoreBluetooth UUID
    # (E621E1F8-C36C-495A-93FC-0C247A3E6E5F).
    if re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", addr) or re.fullmatch(
        r"[0-9A-Fa-f]{8}-(?:[0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}", addr
    ):
        return addr
    log("Cached address malformed, discarding")
    SAVED_ADDR_FILE.unlink(missing_ok=True)
    return None


# --- macOS: recover a device the OS already holds as an HID keyboard --------
#
# The firmware advertises as a BLE HID keyboard so its buttons type into the
# Mac. macOS auto-connects to that HID, and CoreBluetooth then EXCLUDES the
# peripheral from BleakScanner.discover() results (already-connected devices
# never appear in scans). bleak's connect-by-address path also scans
# internally, so a cached address can't help either. The documented escape
# hatch is retrieveConnectedPeripheralsWithServices_, which returns
# peripherals the system is already connected to. We wrap the result in a
# BLEDevice carrying the live (peripheral, manager) details so BleakClient
# connects to it directly without scanning. CoreBluetooth shares the single
# physical link, so this rides the existing HID connection — the keyboard
# keeps working.
_cb_manager = None  # reused CentralManagerDelegate (CoreBluetooth)


async def _get_cb_manager():
    """Lazily create and ready a shared CoreBluetooth central manager."""
    global _cb_manager
    if _cb_manager is None:
        from bleak.backends.corebluetooth.CentralManagerDelegate import (
            CentralManagerDelegate,
        )

        mgr = CentralManagerDelegate()
        await mgr.wait_until_ready()  # raises if Bluetooth is unauthorized/off
        _cb_manager = mgr
    return _cb_manager


async def retrieve_connected_macos(skip_addr: str | None = None):
    """Return a BLEDevice for a system-connected 'Clawdmeter', or None.

    Two-step lookup, strongest signal first:

    1. Peripherals connected under our CUSTOM service UUID. Membership in
       that service is unambiguous (no other device exposes it), so we accept
       by service alone — the peripheral's name can be None on macOS.
    2. Fall back to the generic HID service 0x1812, but ONLY trust a
       peripheral whose name matches DEVICE_NAME. 0x1812 also matches
       unrelated keyboards/mice, so picking blindly here could grab the
       wrong device.

    ``skip_addr`` skips a peripheral whose UUID just failed to connect, so a
    stale CoreBluetooth handle can't trap us into never trying a fresh scan.
    """
    from CoreBluetooth import CBUUID
    from bleak.backends.device import BLEDevice

    try:
        manager = await _get_cb_manager()
    except Exception as e:  # BleakBluetoothNotAvailableError etc.
        log(f"CoreBluetooth unavailable: {e}")
        return None

    cm = manager.central_manager

    def _wrap(p):
        addr = p.identifier().UUIDString()
        log(f"Found system-connected peripheral: {p.name()!r} [{addr}]")
        return BLEDevice(addr, p.name(), (p, manager))

    def _ok(p) -> bool:
        return not (skip_addr and p.identifier().UUIDString() == skip_addr)

    # 1. Custom service — accept by service membership alone.
    custom = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_(SERVICE_UUID)]
    )
    for p in custom or []:
        if _ok(p):
            return _wrap(p)

    # 2. Generic HID service — require an exact name match.
    hid = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_("1812")]
    )
    for p in hid or []:
        if _ok(p) and p.name() == DEVICE_NAME:
            return _wrap(p)

    return None


async def discover_target(skip_addr: str | None = None):
    """Return a connectable target, or None.

    The daemon only ever targets the device this system already holds — it
    never scans for a nearby device by name, so it can't grab a stranger's or
    the wrong nearby unit. On macOS that's the system-connected peripheral (the
    firmware advertises as an HID keyboard, so once paired the OS auto-connects
    and holds it — HID-grabbed devices are invisible to scans anyway). On other
    platforms it's a previously-pinned address in the cache file. If the device
    isn't held/pinned, we log and wait rather than scanning. ``skip_addr`` skips
    a peripheral whose handle just failed to connect.
    """
    if sys.platform == "darwin":
        dev = await retrieve_connected_macos(skip_addr=skip_addr)
        if dev is None:
            log("Device not held by OS; waiting (not scanning by name)")
        return dev

    address = load_cached_address()
    if not address:
        log("No pinned address cached; waiting (not scanning by name)")
    return address


def read_chime_setting() -> str:
    """Read the `chime` option from the config file. One of: off|on.

    Defaults to "off" (the device stays silent) so existing setups are
    unaffected until the user opts in.
    """
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "chime":
                    val = val.strip().lower()
                    if val in ("off", "on"):
                        return val
    except OSError:
        pass
    return "off"


def read_clock_setting() -> str:
    """Read the `clock` option from the config file. One of: off|auto|12|24.

    Defaults to "off" (no clock; the device keeps showing "Usage") so existing
    setups are unaffected until the user opts in.
    """
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "clock":
                    val = val.strip().lower()
                    if val in ("off", "auto", "12", "24"):
                        return val
    except OSError:
        pass
    return "off"


def add_chime_field(payload: dict) -> None:
    """Add "c":1 to the payload when the config opts in, so the firmware may
    sound the session-reset chime. Omitted entirely when chime is off."""
    if read_chime_setting() == "on":
        payload["c"] = 1


def detect_hour_format() -> int:
    """Best-effort 12h/24h detection for the host. Returns 12 or 24 (default 24)."""
    # macOS: the explicit System Settings toggle lives in NSGlobalDomain.
    for key, result in (("AppleICUForce24HourTime", 24), ("AppleICUForce12HourTime", 12)):
        try:
            out = subprocess.run(["defaults", "read", "-g", key],
                                 capture_output=True, text=True, timeout=3)
            if out.stdout.strip() == "1":
                return result
        except (OSError, subprocess.SubprocessError):
            pass
    # Fallback to the C locale's time format (may be C/24h under launchd).
    try:
        import locale
        locale.setlocale(locale.LC_TIME, "")
        fmt = locale.nl_langinfo(locale.T_FMT)
        if "%p" in fmt or "%r" in fmt or "%I" in fmt:
            return 12
    except (ImportError, locale.Error, AttributeError):
        pass
    return 24


def add_clock_fields(payload: dict) -> None:
    """Add wall-clock fields to the payload when the config opts in.

    "t"  = local wall-clock epoch (UTC epoch shifted by the tz offset) so the
           device can show the time without an RTC.
    "tf" = 12 or 24, the hour format the device should render.
    """
    clock = read_clock_setting()
    if clock == "off":
        return
    tf = 24 if clock == "24" else 12 if clock == "12" else detect_hour_format()
    payload["t"] = int(time.time()) + time.localtime().tm_gmtoff
    payload["tf"] = tf


# ---- Running agents ----
# Claude Code keeps a registry of live sessions at <config_dir>/sessions/<pid>.json
# ({"pid", "name", "cwd", "status": "busy"|"idle"|…, "statusUpdatedAt" ms, …}).
# Files can outlive a crashed process, so each entry is kept only while its PID
# is alive. The list goes to the device as its own payload, separate from usage:
#   {"ag": [[name, state, mins_in_state, activity], …], "n": total_running}
# state: "b" busy/working, "w" waiting on the user, "d" done (went busy →
# idle within the last DONE_HOLD_S), "i" idle (Claude Code's
# statuses are busy / waiting / idle, plus "shell" = idle with a shell open,
# which falls through to idle here). activity is a
# short hint of what a busy/waiting session is doing ("Edit ui.cpp"), read from
# the tail of its transcript; "" when idle or unknown.
AGENTS_MAX = 4            # rows the device can show
AGENT_NAME_MAX = 16       # device-side name buffer is 20 bytes; keep it short
AGENT_ACT_MAX = 18        # device-side activity buffer is 20 bytes
AGENTS_PAYLOAD_MAX = 180  # default write-without-response budget (MTU 185 - 3)
TRANSCRIPT_TAIL = 65536   # bytes of transcript read to find the latest action
AGENTS_RESEND_S = 60      # re-send an unchanged list so the device's 90s freshness never lapses
AGENTS_WATCH_S = 0.5      # registry change check; Claude Code rewrites it on every status flip
DONE_HOLD_S = 60          # how long a just-finished session shows as "done"
STATE_ORDER = "wdbi"      # rows: waiting, done, working, idle


def read_agents_setting() -> str:
    """Read the `agents` option from the config file. One of: off|on. Default on."""
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "agents":
                    val = val.strip().lower()
                    if val in ("off", "on"):
                        return val
    except OSError:
        pass
    return "on"


def _pid_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True   # exists, owned by someone else
    except (OSError, ValueError):
        return False
    return True


def _agent_state(status: str) -> str:
    s = (status or "").lower()
    if s == "busy":
        return "b"
    if "wait" in s or "permission" in s or "input" in s:
        return "w"
    return "i"


def _short(text: str, n: int) -> str:
    text = " ".join(str(text).split())
    return text if len(text) <= n else text[: n - 2].rstrip() + ".."


SHELL_PREFIX_WORDS = {"do", "then", "else", "elif", "{", "(", "!", "time"}
SHELL_SKIP_WORDS = {"cd", "for", "while", "until", "if", "case", "select",
                    "done", "fi", "esac", "}", ")"}


def describe_tool(name: str, inp: dict) -> str:
    """One short phrase for a tool call, e.g. "Edit ui.cpp", "Run pio"."""
    inp = inp if isinstance(inp, dict) else {}
    path = inp.get("file_path") or inp.get("notebook_path") or inp.get("path")
    if name in ("Edit", "MultiEdit", "Write", "Read", "NotebookEdit") and path:
        verb = "Edit" if name in ("MultiEdit", "NotebookEdit") else name
        return f"{verb} {Path(str(path)).name}"
    if name == "Bash":
        # First real program: skip `cd …` segments, env assignments, sudo, and
        # shell syntax like `$(` so "cd x && P=$(ls y) && pio run" → "Run pio".
        for seg in re.split(r"&&|\|\||;|\||\n", str(inp.get("command", ""))):
            words = []
            for w in seg.split():
                if "=" in w:   # VAR=value is skipped; VAR=$(cmd … runs cmd
                    if "$(" not in w:
                        continue
                    w = w.split("$(", 1)[1]
                w = w.lstrip("$(`")
                if w and w != "sudo":
                    words.append(w)
            # Shell control flow: `for x in …` / `while cond` headers aren't
            # programs, and `do`/`then` prefix the body's first command.
            while words and words[0] in SHELL_PREFIX_WORDS:
                words.pop(0)
            if words and words[0] not in SHELL_SKIP_WORDS:
                return f"Run {Path(words[0]).name}"
        return "Run command"
    if name in ("Grep", "Glob"):
        return f"Search {inp.get('pattern', '')}".strip()
    if name in ("Agent", "Task"):
        return "Subagent"
    if name == "WebSearch":
        return "Web search"
    if name == "WebFetch":
        return "Fetch web page"
    if name.startswith("mcp__"):
        return name.rsplit("__", 1)[-1].replace("_", " ")
    return name


def last_activity(transcript: Path) -> str:
    """What the session is doing now, from the newest user/assistant entry."""
    try:
        with transcript.open("rb") as f:
            f.seek(0, os.SEEK_END)
            f.seek(max(0, f.tell() - TRANSCRIPT_TAIL))
            lines = f.read().decode("utf-8", "replace").splitlines()
    except OSError:
        return ""
    for line in reversed(lines):
        try:
            entry = json.loads(line)
        except ValueError:
            continue   # first line of the tail is usually cut mid-way
        if entry.get("type") not in ("assistant", "user"):
            continue
        content = (entry.get("message") or {}).get("content")
        if entry["type"] == "user":
            return "Thinking"   # just got a prompt or a tool result back
        if not isinstance(content, list) or not content:
            return "Thinking"
        block = content[-1]
        kind = block.get("type")
        if kind == "tool_use":
            return describe_tool(block.get("name", "Tool"), block.get("input"))
        return "Writing" if kind == "text" else "Thinking"
    return ""


_TRANSCRIPTS: dict[str, Path] = {}


def find_transcript(config_dir: Path, session_id: str) -> Path | None:
    """<config_dir>/projects/<encoded cwd>/<session_id>.jsonl, cached."""
    if not session_id:
        return None
    hit = _TRANSCRIPTS.get(session_id)
    if hit and hit.exists():
        return hit
    for path in (config_dir / "projects").glob(f"*/{session_id}.jsonl"):
        _TRANSCRIPTS[session_id] = path
        return path
    return None


def collect_agents(config_dirs: list[Path] | None = None,
                   now_ms: float | None = None) -> list[tuple[str, str, int, str]]:
    """Live Claude Code sessions as (name, state, minutes_in_state, activity),
    most relevant first: working, then waiting, then idle; newest change first."""
    dirs = config_dirs if config_dirs is not None else read_config_dirs()
    now_ms = now_ms if now_ms is not None else time.time() * 1000
    seen: set[int] = set()
    rows: list[tuple[int, float, str, str, int, str]] = []
    for d in dirs:
        for f in (d / "sessions").glob("*.json"):
            try:
                info = json.loads(f.read_text())
                pid = int(info.get("pid") or f.stem)
            except (OSError, ValueError, TypeError):
                continue
            if pid in seen or not _pid_alive(pid):
                continue
            seen.add(pid)
            name = info.get("name") or Path(info.get("cwd") or "?").name or "claude"
            # Device fonts are ASCII-only (0x20-0x7E)
            name = "".join(c if " " <= c <= "~" else "?" for c in str(name))
            state = _agent_state(info.get("status", ""))
            since = info.get("statusUpdatedAt") or info.get("startedAt") or now_ms
            try:
                mins = max(0, int((now_ms - float(since)) // 60000))
            except (TypeError, ValueError):
                mins = 0
            act = ""
            if state != "i":
                transcript = find_transcript(d, str(info.get("sessionId") or ""))
                if transcript:
                    act = last_activity(transcript)
            # Claude Code records why a session is waiting: "permission prompt"
            # (the pending tool call is what needs approving) or "input needed".
            waiting_for = str(info.get("waitingFor") or "")
            if state == "w" and "input" in waiting_for:
                act = "Needs your input"
            elif state == "w" and act and act not in ("Thinking", "Writing"):
                act = f"Allow {act}"
            act = "".join(c if " " <= c <= "~" else "?" for c in act)
            rows.append((STATE_ORDER.index(state), -float(since), name, state, mins, act))
    rows.sort()
    return [(name, state, mins, act) for _, _, name, state, mins, act in rows]


def sessions_signature(config_dirs: list[Path] | None = None) -> tuple:
    """Cheap fingerprint of the session registries (names + mtimes). Claude Code
    rewrites a session's file on every status change, so a new signature means
    a session started, stopped, finished or began waiting."""
    dirs = config_dirs if config_dirs is not None else read_config_dirs()
    sig = []
    for d in dirs:
        for f in (d / "sessions").glob("*.json"):
            try:
                sig.append((str(f), f.stat().st_mtime_ns))
            except OSError:
                pass
    return tuple(sorted(sig))


class DoneTracker:
    """Marks sessions that just went busy → idle as "d" (done) for DONE_HOLD_S,
    so the device can flag "finished" instead of a plain idle row."""

    def __init__(self) -> None:
        self.prev: dict[str, str] = {}
        self.done_at: dict[str, float] = {}

    def apply(self, agents: list[tuple[str, str, int, str]],
              now: float | None = None) -> list[tuple[str, str, int, str]]:
        now = now if now is not None else time.time()
        out = []
        for name, state, mins, act in agents:
            if state == "i" and self.prev.get(name) == "b":
                self.done_at[name] = now
            elif state != "i":
                self.done_at.pop(name, None)
            self.prev[name] = state
            if state == "i" and now - self.done_at.get(name, -1e9) < DONE_HOLD_S:
                state, act = "d", "Finished"
            out.append((name, state, mins, act))
        live = {a[0] for a in agents}
        for gone in set(self.prev) - live:   # session exited
            self.prev.pop(gone, None)
            self.done_at.pop(gone, None)
        out.sort(key=lambda a: STATE_ORDER.index(a[1]))   # stable: keeps recency order
        return out


def agents_payload(agents: list[tuple[str, str, int, str]],
                   limit: int = AGENTS_PAYLOAD_MAX) -> dict:
    """Device payload for the agents page, trimmed to fit one BLE write of
    ``limit`` bytes: activity hints shrink first, then vanish, then rows drop."""
    top = agents[:AGENTS_MAX]

    def build(act_max: int) -> dict:
        rows = []
        for n, s, m, a in top:
            row = [n[:AGENT_NAME_MAX], s, min(m, 9999)]
            if a and act_max:
                row.append(_short(a, act_max))
            rows.append(row)
        return {"ag": rows, "n": len(agents)}

    def size(p: dict) -> int:
        return len(json.dumps(p, separators=(",", ":")).encode())

    for act_max in (AGENT_ACT_MAX, 10, 0):
        payload = build(act_max)
        if size(payload) <= limit:
            return payload
    while payload["ag"] and size(payload) > limit:
        payload["ag"].pop()
    return payload


async def poll_api(token: str) -> dict | None:
    headers = dict(API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.post(API_URL, headers=headers, json=API_BODY)
    except httpx.HTTPError as e:
        log(f"API call failed: {e}")
        return None
    if resp.status_code in (401, 403):
        log(f"API HTTP {resp.status_code} (token expired/invalid)")
        raise TokenExpired()
    if resp.status_code == 429:
        # Retrying fast keeps the account rate-limited; back off.
        global _rate_limited_until
        try:
            wait = float(resp.headers.get("retry-after", "0"))
        except ValueError:
            wait = 0.0
        wait = max(wait, RATE_LIMIT_MIN_S)
        _rate_limited_until = time.time() + wait
        log(f"API HTTP 429 (rate limited); next poll in {int(wait)}s")
        return None
    if resp.status_code >= 400:
        log(f"API HTTP {resp.status_code}: {resp.text[:200]}")
        return None

    def hdr(name: str, default: str = "0") -> str:
        return resp.headers.get(name, default)

    now = time.time()

    def reset_minutes(reset_ts: str) -> int:
        try:
            r = float(reset_ts)
        except ValueError:
            return 0
        mins = (r - now) / 60.0
        return int(round(mins)) if mins > 0 else 0

    def pct(util: str) -> int:
        try:
            return int(round(float(util) * 100))
        except ValueError:
            return 0

    # Pro/Max accounts expose 5h/7d windows; Enterprise/overage use a single
    # spending-limit model reported via overage-utilization.
    if resp.headers.get("anthropic-ratelimit-unified-5h-utilization"):
        payload = {
            "s": pct(hdr("anthropic-ratelimit-unified-5h-utilization")),
            "sr": reset_minutes(hdr("anthropic-ratelimit-unified-5h-reset")),
            "w": pct(hdr("anthropic-ratelimit-unified-7d-utilization")),
            "wr": reset_minutes(hdr("anthropic-ratelimit-unified-7d-reset")),
            "st": hdr("anthropic-ratelimit-unified-5h-status", "unknown"),
            "acct": "pro",
            "ok": True,
        }
    else:
        reset_ts = hdr("anthropic-ratelimit-unified-overage-reset")
        payload = {
            "s": pct(hdr("anthropic-ratelimit-unified-overage-utilization")),
            "sr": reset_minutes(reset_ts),
            "w": 0,
            "wr": 0,
            "st": hdr("anthropic-ratelimit-unified-status", "unknown"),
            "acct": "ent",
            **_billing_period_info(now, reset_ts),
            "ok": True,
        }
    add_chime_field(payload)   # adds "c":1 iff the config opts in
    add_clock_fields(payload)   # adds "t" + "tf" iff the config opts in
    return payload


def _billing_period_info(now: float, reset_ts: str) -> dict:
    """Fraction of billing period elapsed (tp, 0-100) and period length in days (pd).

    Billing periods are assumed calendar-monthly: period_end is the reset
    timestamp, period_start is the same day/time one calendar month earlier.

    The rate-limit headers expose only the reset timestamp, not the period
    length, so the monthly window is an assumption — but a documented one:
    Enterprise spend-limit `period` "the only value today is monthly"
    (Claude Enterprise Admin API reference). The doc notes period is an open
    string that may gain other values later; revisit this if so.
    """
    try:
        period_end = float(reset_ts)
    except ValueError:
        return {"tp": 0, "pd": 30}
    if period_end <= 0:
        # reset_ts defaults to "0" when the overage-reset header is absent.
        # fromtimestamp(0) is 1970; stepping a month back lands in 1969, and
        # datetime.timestamp() raises OSError for pre-1970 dates on Windows.
        # Benign on macOS/Linux, but guard here too to keep the daemons parallel.
        return {"tp": 0, "pd": 30}
    dt_end = datetime.datetime.fromtimestamp(period_end)
    prev_month = dt_end.month - 1 or 12
    prev_year = dt_end.year if dt_end.month > 1 else dt_end.year - 1
    prev_day = min(dt_end.day, calendar.monthrange(prev_year, prev_month)[1])
    dt_start = dt_end.replace(year=prev_year, month=prev_month, day=prev_day)
    period_start = dt_start.timestamp()
    period_len = period_end - period_start
    if period_len <= 0:
        return {"tp": 0, "pd": 30}
    pct_val = (now - period_start) / period_len * 100
    total_days = int(round(period_len / 86400))
    rd = f"{dt_end.strftime('%b')} {dt_end.day}"
    return {
        "tp": max(0, min(100, int(round(pct_val)))),
        "pd": total_days,
        "rd": rd,
    }


class PlanSelector:
    """Decide which config dir's plan is "active" across polls.

    "Active" = the plan whose session % rose most recently (recent API activity).
    A rise stamps a monotonic poll counter, so the choice is sticky and a window
    reset (a drop to 0) isn't mistaken for use. Before any rise is seen (startup)
    the highest current session % wins. Mirrors the Linux bash daemon.
    """

    def __init__(self) -> None:
        self.prev_s: dict[Path, int] = {}
        self.last_active: dict[Path, int] = {}
        self.seq = 0

    def choose(self, sessions: dict[Path, int]) -> Path:
        """Update state from this cycle's {dir: session_pct} and return the active dir."""
        self.seq += 1
        for d, s in sessions.items():
            if d in self.prev_s and s > self.prev_s[d]:
                self.last_active[d] = self.seq
            self.prev_s[d] = s
        # Most recent activity wins; ties (and the startup case) break by highest %.
        return max(sessions, key=lambda d: (self.last_active.get(d, 0), sessions[d]))


# Module-level so the active-plan state survives reconnects.
_SELECTOR = PlanSelector()


async def poll_active(selector: PlanSelector = _SELECTOR) -> tuple[dict | None, bool]:
    """Poll every configured config dir; return ``(active_payload, all_dead)``.

    ``active_payload`` — the active plan's payload dict, or None when no dir
    yields a usable payload this cycle. A single configured dir (the default)
    collapses to exactly the old single-poll path.

    ``all_dead`` — True when *every* configured dir lacked a usable token this
    cycle (file/Keychain empty, or a 401/expired token), so the caller can
    signal "No data". False when at least one token authenticated — including a
    transient non-auth poll failure worth retrying silently rather than idling.

    Pure free-ride: a 401 (TokenExpired) means that dir's token has expired and
    only Claude Code (its owner) can re-seed it — we never refresh it ourselves.
    """
    dirs = read_config_dirs()
    payloads: dict[Path, dict] = {}
    sessions: dict[Path, int] = {}
    any_live = False
    for d in dirs:
        token = read_token_for(d)
        if not token:
            log(f"No token in {d}; skipping")
            continue
        try:
            payload = await poll_api(token)
        except TokenExpired:
            log(f"Token in {d} expired/invalid; skipping")
            continue
        # Authenticated: a transient None here isn't an auth failure, so the
        # dir counts as live and we stay silent rather than idling the device.
        any_live = True
        if payload is not None:
            payloads[d] = payload
            sessions[d] = int(payload.get("s", 0) or 0)
    if not payloads:
        return None, not any_live
    active = selector.choose(sessions)
    if len(dirs) > 1:
        log(f"Active plan: {active} (s={sessions[active]})")
    return payloads[active], False


async def poll_active_payload(selector: PlanSelector = _SELECTOR) -> dict | None:
    """The active plan's payload, or None when no dir yields one this cycle.

    Thin wrapper over :func:`poll_active` for callers that don't need the
    all-dead flag.
    """
    payload, _dead = await poll_active(selector)
    return payload


class Session:
    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self.refresh_requested = asyncio.Event()

    def _on_refresh(self, _char, _data: bytearray) -> None:
        log("Refresh requested by device")
        self.refresh_requested.set()

    async def setup_refresh_subscription(self) -> None:
        # start_notify awaits CoreBluetooth's CCCD-write confirmation, which
        # never arrives if the peripheral doesn't ACK the subscribe (a
        # half-open link after the OS auto-connects the HID). Unbounded, that
        # await wedges the whole daemon between "Connected" and the first poll
        # — the device then shows nothing until a manual restart. Bound it: the
        # subscription is only an optional device-initiated refresh nudge (we
        # poll every POLL_INTERVAL regardless), so on timeout we proceed.
        try:
            await asyncio.wait_for(
                self.client.start_notify(REQ_CHAR_UUID, self._on_refresh),
                timeout=10,
            )
        except (BleakError, ValueError) as e:
            log(f"Refresh subscription unavailable: {e}")
        except asyncio.TimeoutError:
            log("Refresh subscription timed out; polling without it")

    def write_limit(self) -> int:
        """Largest single write-without-response the link allows (bytes)."""
        try:
            char = self.client.services.get_characteristic(RX_CHAR_UUID)
            n = int(char.max_write_without_response_size)
            if n >= 20:
                return n
        except (AttributeError, BleakError, TypeError, ValueError):
            pass
        return AGENTS_PAYLOAD_MAX

    async def write_payload(self, payload: dict) -> bool:
        data = json.dumps(payload, separators=(",", ":")).encode()
        log(f"Sending: {data.decode()}")
        try:
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=False)
            return True
        except BleakError as e:
            log(f"Write failed: {e}")
            return False


def _is_encryption_error(exc: BaseException) -> bool:
    """True if a connect error is a macOS bonding/encryption mismatch.

    macOS reports a stale bond as CBErrorDomain Code=15 ("Failed to encrypt
    the connection..."). Match on the message text so we don't depend on how
    bleak wraps the underlying CoreBluetooth error.
    """
    s = str(exc).lower()
    return "code=15" in s or "encrypt" in s


# blueutil talks to Bluetooth via IOBluetooth, which on recent macOS needs its
# OWN Bluetooth TCC grant (separate from the daemon's CoreBluetooth grant).
# Without it, blueutil *hangs* instead of erroring — so every call is bounded
# by a timeout and a hang is reported as a permission problem, not a crash.
BLUEUTIL_TIMEOUT = 8


def _blueutil(*args: str) -> str | None:
    """Run `blueutil <args>`, returning stdout, or None on failure/timeout.

    A timeout almost always means blueutil lacks Bluetooth permission (it
    blocks rather than failing), so we surface that cause explicitly.
    """
    try:
        return subprocess.run(
            ["blueutil", *args],
            capture_output=True, text=True,
            timeout=BLUEUTIL_TIMEOUT, check=True,
        ).stdout
    except subprocess.TimeoutExpired:
        log(f"blueutil {' '.join(args)} timed out — it likely lacks Bluetooth "
            "permission. Grant it under System Settings > Privacy & Security > "
            "Bluetooth (run `blueutil --paired` once from Terminal to prompt).")
        return None
    except (subprocess.SubprocessError, OSError) as e:
        log(f"blueutil {' '.join(args)} failed: {e}")
        return None


def unpair_macos() -> bool:
    """Forget a stale macOS bond for DEVICE_NAME so the device can re-pair.

    A Code=15 "failed to encrypt" connect error means macOS holds bonding
    keys that no longer match the ESP32's (e.g. after a firmware reflash or
    the on-device bond-clear gesture). The firmware pairs "just works" (no
    MITM), so once the stale bond is gone the next connect re-bonds silently
    with no GUI prompt.

    CoreBluetooth exposes no unpair API, so we shell out to `blueutil`. The
    daemon only knows the peripheral's CoreBluetooth UUID, not the BD_ADDR
    that blueutil needs, so we map by name via `blueutil --paired`. Returns
    True if a bond was removed. Mirrors the Linux daemon's `bluetoothctl
    remove` self-heal.
    """
    if not shutil.which("blueutil"):
        log("Stale bond detected but `blueutil` is not installed; cannot "
            "auto-recover. Run `brew install blueutil`, or forget "
            f"'{DEVICE_NAME}' in System Settings > Bluetooth and reconnect.")
        return False

    out = _blueutil("--paired")
    if out is None:
        return False

    # Each line looks like:
    #   address: 28-84-85-55-5c-3d, ... name: "Clawdmeter", ...
    addr = None
    for line in out.splitlines():
        if f'name: "{DEVICE_NAME}"' in line:
            m = re.search(r"address:\s*([0-9a-fA-F:-]+)", line)
            if m:
                addr = m.group(1)
                break
    if not addr:
        log(f"No paired '{DEVICE_NAME}' found to unpair (already forgotten?)")
        return False

    if _blueutil("--unpair", addr) is None:
        return False
    log(f"Unpaired stale bond for '{DEVICE_NAME}' [{addr}]; re-pairing on "
        "next connect")
    return True


async def connect_and_run(target, stop_event: asyncio.Event) -> bool:
    """Connect to a target and poll until disconnected or stopped.

    ``target`` is either an address string (Linux) or a BLEDevice carrying
    live CoreBluetooth details (macOS). Returns True if the connection was
    used successfully (so the caller keeps the cached address), False if the
    connection failed and the cache should be invalidated.
    """
    display = target if isinstance(target, str) else target.address
    log(f"Connecting to {display}...")
    client = BleakClient(target)
    try:
        # Bound the connect the same way #84 bounded the refresh subscribe.
        # On macOS the OS auto-connects the firmware's HID link, so
        # CoreBluetooth can hand us a half-open peripheral whose GATT connect
        # handshake never completes. BleakClient's own timeout governs
        # discovery, not connectPeripheral, so an unbounded await here wedges
        # the single-threaded daemon forever at "Connecting..." (observed ~13h,
        # device stuck on stale data). wait_for raises TimeoutError, which the
        # handler below already treats as a connection failure -> drop the
        # cached address and rescan.
        await asyncio.wait_for(client.connect(), timeout=CONNECT_TIMEOUT)
    except (BleakError, asyncio.TimeoutError) as e:
        log(f"Connection failed: {e}")
        if sys.platform == "darwin" and _is_encryption_error(e):
            log("Encryption failed — likely a stale macOS bond; self-healing")
            unpair_macos()
        return False

    if not client.is_connected:
        log("Connection failed (no error but not connected)")
        return False

    log("Connected")
    session = Session(client)
    await session.setup_refresh_subscription()
    log(f"Write budget: {session.write_limit()} bytes")

    last_poll = 0.0
    last_payload: dict | None = None   # last good usage payload…
    last_payload_at = 0.0              # …when it was fetched
    last_usage_write = 0.0             # last usage write of any kind (poll or heartbeat)
    last_agents: dict | None = None
    last_agents_sent = 0.0
    tracker = DoneTracker()
    used_successfully = False

    async def push_agents() -> None:
        # Write only on a change (or the periodic resend that keeps the
        # device's copy fresh).
        nonlocal last_agents, last_agents_sent
        if read_agents_setting() != "on":
            return
        agents = agents_payload(tracker.apply(collect_agents()), session.write_limit())
        if agents != last_agents or time.time() - last_agents_sent >= AGENTS_RESEND_S:
            if await session.write_payload(agents):
                last_agents = agents
                last_agents_sent = time.time()

    try:
        while client.is_connected and not stop_event.is_set():
            # Full agents scan every tick (activity hints come from transcripts)…
            await push_agents()

            now = time.time()
            elapsed = now - last_poll
            if now < _rate_limited_until:
                pass   # backing off after a 429; heartbeats below keep the device live
            elif session.refresh_requested.is_set() or elapsed >= POLL_INTERVAL:
                session.refresh_requested.clear()
                # Pure free-ride: read whatever access token(s) Claude Code
                # currently holds across the configured config dirs and NEVER
                # refresh them ourselves. Claude Code (the token's owner) does all
                # refreshing; refreshing here would race its rotation and feed the
                # OAuth endpoint's rate limit (429). When no dir has a usable token
                # we signal "No data" so the device idles instead of holding stale
                # numbers until the CLI re-seeds it.
                payload, dead = await poll_active()
                if payload is not None:
                    if await session.write_payload(payload):
                        last_poll = time.time()
                        last_payload, last_payload_at = payload, last_poll
                        last_usage_write = last_poll
                        used_successfully = True
                elif dead:
                    # No live token in any config dir (missing, or a 401/expired
                    # token) -> show "No data" now instead of stale numbers. Guard
                    # last_poll on the write result (like the data path) so a
                    # failed beat retries next tick instead of throttling what may
                    # be a healthy link for a full POLL_INTERVAL.
                    log("No usable token; signalling no-data to device — run "
                        "`claude login` or use the CLI to let Claude Code renew it")
                    if await session.write_payload({"ok": False}):
                        last_poll = time.time()
                        last_payload = None   # never heartbeat over a no-data beat
                else:
                    # Transient poll failure (a live token that didn't answer this
                    # cycle, or a 429) -> retry in RETRY_AFTER_FAILURE_S rather
                    # than every tick; hammering is what keeps a 429 going.
                    log("No usable config dir this cycle")
                    last_poll = time.time() - POLL_INTERVAL + RETRY_AFTER_FAILURE_S

            # Heartbeat: while polls fail, replay the last good numbers (aged) so
            # the device's 90s freshness window doesn't lapse into "No data" —
            # but only for HEARTBEAT_MAX_AGE_S, after which they're too old to show.
            now = time.time()
            if (last_payload is not None and now - last_usage_write >= HEARTBEAT_S
                    and now - last_payload_at <= HEARTBEAT_MAX_AGE_S):
                aged = age_payload(last_payload, now - last_payload_at)
                log("Heartbeat (replaying last usage)")
                if await session.write_payload(aged):
                    last_usage_write = now

            # …and between ticks, react within AGENTS_WATCH_S when the session
            # registry changes (finished / waiting / started / exited).
            sig = sessions_signature()
            deadline = time.time() + TICK
            while time.time() < deadline and client.is_connected and not stop_event.is_set():
                try:
                    await asyncio.wait_for(session.refresh_requested.wait(), timeout=AGENTS_WATCH_S)
                    break
                except asyncio.TimeoutError:
                    pass
                new_sig = sessions_signature()
                if new_sig != sig:
                    sig = new_sig
                    await push_agents()
    finally:
        try:
            await client.disconnect()
        except BleakError:
            pass

    log("Device disconnected" if not stop_event.is_set() else "Stopping")
    return used_successfully


async def main() -> None:
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _stop)
        except NotImplementedError:
            signal.signal(sig, _stop)

    log("=== Claude Usage Tracker Daemon (BLE, macOS) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")

    backoff = 1
    skip_addr: str | None = None  # macOS: a peripheral to skip for one cycle
    while not stop_event.is_set():
        # Apply any pending skip exactly once, then clear it so the next
        # cycle re-tries retrieveConnected (the device may have recovered).
        target = await discover_target(skip_addr=skip_addr)
        skip_addr = None
        if not target:
            log(f"Device not found, retrying in {backoff}s...")
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
            continue

        addr = target if isinstance(target, str) else target.address
        ok = await connect_and_run(target, stop_event)
        if not ok:
            if sys.platform == "darwin":
                # No string cache to drop; instead skip this stale handle on
                # the next retrieveConnected so the scan fallback is reachable.
                skip_addr = addr
            else:
                log("Invalidating cached address")
                SAVED_ADDR_FILE.unlink(missing_ok=True)
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
        else:
            backoff = 1


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
