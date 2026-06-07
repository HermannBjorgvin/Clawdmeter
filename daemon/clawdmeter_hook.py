#!/usr/bin/env python3
"""Claude Code hook entry point for Clawdmeter.

Registered in ~/.claude/settings.json as a `hooks` entry. On every
hook event Claude Code invokes this script and pipes a JSON payload to
stdin. We extract the bits the daemon cares about (todos, current
tool, model, cwd) into a per-session record in ~/.clawdmeter/state.json
and exit silently (fail-open: a stdout JSON `{}` lets Claude proceed
unimpeded).

The daemon polls state.json on every tick and merges its contents into
the BLE payload sent to the ESP32.
"""

import fcntl
import json
import os
import sys
import time
from pathlib import Path

STATE_DIR = Path.home() / ".clawdmeter"
STATE_FILE = STATE_DIR / "state.json"
LOCK_FILE = STATE_DIR / "state.lock"

# Cap session retention so a forgotten session doesn't permanently
# occupy a slot in the device UI. Anything older than this gets pruned
# on the next hook fire.
SESSION_TTL_SECONDS = 15 * 60

# Context-window sizes by model. The transcript logs the model WITHOUT the
# 1M-variant marker, so this is best-effort; the safety rule in _compute_ctx_pct
# bumps to 1M when observed tokens already exceed the mapped window.
MODEL_WINDOW = {
    "claude-opus-4-8": 1_000_000, "claude-opus-4-7": 1_000_000, "claude-opus-4-6": 1_000_000,
    "claude-sonnet-4-6": 1_000_000,
    "claude-sonnet-4-5": 200_000, "claude-sonnet-4": 200_000,
    "claude-haiku-4-5": 200_000,
}
DEFAULT_WINDOW = 200_000


def _now() -> int:
    return int(time.time())


def _short_model(model: str | None) -> str:
    """Strip the date suffix so 'claude-sonnet-4-6-20250930' → 'sonnet-4-6'."""
    if not model:
        return ""
    name = model.replace("claude-", "")
    parts = name.split("-")
    # Drop trailing date-shaped chunk (8 digits)
    if parts and parts[-1].isdigit() and len(parts[-1]) >= 8:
        parts = parts[:-1]
    return "-".join(parts)


def _short_project(cwd: str | None) -> str:
    if not cwd:
        return ""
    return Path(cwd).name


def _ctx_window_for(raw_model: str) -> int:
    """Map a raw transcript model id (e.g. 'claude-opus-4-8-20260101') to its
    context-window size. Strips a trailing date chunk; keeps the 'claude-'
    prefix (MODEL_WINDOW keys include it)."""
    if not raw_model:
        return DEFAULT_WINDOW
    parts = raw_model.split("-")
    if parts and parts[-1].isdigit() and len(parts[-1]) >= 8:
        parts = parts[:-1]
    return MODEL_WINDOW.get("-".join(parts), DEFAULT_WINDOW)


def _compute_ctx_pct(transcript_path):
    """Approximate context-window % from the latest assistant turn's usage in
    the session transcript. Returns an int 0..100, or None on any problem
    (fail-open). Reads the RAW transcript model (with 'claude-' prefix), not the
    shortened session['model']."""
    if not transcript_path:
        return None
    try:
        last_usage = None
        last_model = ""
        with open(transcript_path) as fh:
            for line in fh:
                try:
                    o = json.loads(line)
                except json.JSONDecodeError:
                    continue
                msg = o.get("message") if isinstance(o.get("message"), dict) else None
                if not msg:
                    continue
                u = msg.get("usage")
                if isinstance(u, dict):
                    last_usage = u
                    last_model = msg.get("model") or last_model
        if not last_usage:
            return None
        tokens = (last_usage.get("input_tokens", 0)
                  + last_usage.get("cache_read_input_tokens", 0)
                  + last_usage.get("cache_creation_input_tokens", 0))
        window = _ctx_window_for(last_model)
        if tokens > window:
            window = 1_000_000
        pct = round(100 * tokens / window) if window else 0
        return max(0, min(100, pct))
    except OSError:
        return None


def _tool_args_summary(tool_name: str, tool_input: dict) -> str:
    """Return a short human-readable summary of a tool's most relevant
    arg (the thing you'd want to see next to the tool name on a 1-line
    display). Returns "" when the tool has nothing useful to show.
    """
    if not isinstance(tool_input, dict):
        return ""
    if tool_name == "Bash":
        return str(tool_input.get("command", ""))[:80]
    if tool_name in ("Read", "Write", "Edit"):
        path = str(tool_input.get("file_path", ""))
        return Path(path).name if path else ""
    if tool_name == "NotebookEdit":
        path = str(tool_input.get("notebook_path", ""))
        return Path(path).name if path else ""
    if tool_name in ("Grep", "Glob"):
        return str(tool_input.get("pattern", ""))[:60]
    if tool_name == "Task":
        return str(tool_input.get("description", ""))[:80]
    if tool_name == "WebFetch":
        return str(tool_input.get("url", ""))[:80]
    if tool_name == "WebSearch":
        return str(tool_input.get("query", ""))[:80]
    if tool_name == "SlashCommand":
        return str(tool_input.get("command", ""))[:80]
    return ""


def _prune(sessions: dict, now: int) -> dict:
    return {
        sid: s for sid, s in sessions.items()
        if now - s.get("last_active_ts", 0) < SESSION_TTL_SECONDS
    }


def _update(payload: dict) -> None:
    """Apply a single hook payload to state.json."""
    STATE_DIR.mkdir(parents=True, exist_ok=True)

    session_id = payload.get("session_id") or "unknown"
    event = payload.get("hook_event_name", "")
    tool_name = payload.get("tool_name", "")
    tool_input = payload.get("tool_input") or {}
    now = _now()

    # Open + lock + read-modify-write
    with open(LOCK_FILE, "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            state = json.loads(STATE_FILE.read_text())
            if not isinstance(state, dict) or "sessions" not in state:
                state = {"sessions": {}}
        except (OSError, json.JSONDecodeError):
            state = {"sessions": {}}

        sessions = state.get("sessions", {})
        sessions = _prune(sessions, now)
        session = sessions.get(session_id, {
            "cwd": "",
            "project": "",
            "model": "",
            "last_tool": "",
            "current_tool": "",
            "current_tool_args": "",
            "phase": "idle",
            "last_user_prompt": "",
            "last_active_ts": 0,
            "ctx_pct": 0,
            "todos": [],
        })

        # Always refresh metadata that hook payloads carry.
        if "cwd" in payload:
            session["cwd"] = payload["cwd"]
            session["project"] = _short_project(payload["cwd"])
        if "model" in payload:
            session["model"] = _short_model(payload["model"])
        session["last_active_ts"] = now

        # Context-window % from the session transcript (best-effort, fail-open).
        pct = _compute_ctx_pct(payload.get("transcript_path"))
        if pct is not None:
            session["ctx_pct"] = pct

        if event == "PreToolUse" and tool_name == "TodoWrite":
            # tool_input.todos = [{content, status, activeForm}, ...]
            raw_todos = tool_input.get("todos") or []
            session["todos"] = [
                {
                    "content": str(t.get("content", ""))[:120],
                    "status": str(t.get("status", "pending")),
                    "activeForm": str(t.get("activeForm", ""))[:80],
                }
                for t in raw_todos if isinstance(t, dict)
            ]
            session["last_tool"] = "TodoWrite"
            session["current_tool"] = "TodoWrite"
            session["current_tool_args"] = ""
            session["phase"] = "running"
        elif event == "PreToolUse" and tool_name:
            session["last_tool"] = tool_name
            session["current_tool"] = tool_name
            session["current_tool_args"] = _tool_args_summary(tool_name, tool_input)
            session["phase"] = "running"
        elif event == "PostToolUse":
            # Intentionally keep `current_tool` set: Claude is usually
            # mid-turn between two tool calls and clearing here would
            # cause the headline to flicker to "(idle)" until PreToolUse
            # for the next tool arrives.
            session["phase"] = "running"
        elif event == "UserPromptSubmit":
            prompt = payload.get("prompt", "")
            if isinstance(prompt, str):
                session["last_user_prompt"] = prompt[:120]
            session["phase"] = "running"
            # Clear current_tool — a new prompt starts a fresh turn,
            # any leftover from the previous turn is no longer accurate.
            session["current_tool"] = ""
        elif event == "Stop":
            session["last_tool"] = "idle"
            session["current_tool"] = ""
            session["current_tool_args"] = ""
            session["phase"] = "idle"
        elif event == "SessionStart":
            # Resume / fresh open: assume idle until a tool fires. Avoids
            # a stale "running" sticking around across daemon restarts.
            session["phase"] = "idle"
            session["current_tool"] = ""
            session["current_tool_args"] = ""

        sessions[session_id] = session
        state["sessions"] = sessions

        tmp = STATE_FILE.with_suffix(".tmp")
        tmp.write_text(json.dumps(state, separators=(",", ":")))
        os.replace(tmp, STATE_FILE)


def main() -> int:
    try:
        raw = sys.stdin.read()
        if not raw.strip():
            return 0
        payload = json.loads(raw)
        if isinstance(payload, dict):
            _update(payload)
    except Exception:
        # Fail-open — never block Claude Code on our errors.
        pass
    finally:
        # Empty JSON directive = no-op, let Claude proceed.
        sys.stdout.write("{}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
