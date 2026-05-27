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

import json
import os
import sys
import time
from pathlib import Path

# Cross-platform advisory file lock used by the read-modify-write cycle on
# state.json. fcntl is Unix-only; msvcrt.locking is its Windows counterpart.
# Hooks fire at most ~5x/min and complete in <100ms, so a 10-second contention
# window (msvcrt.LK_LOCK's blocking timeout) is more than enough.
if sys.platform == "win32":
    import msvcrt

    def _flock_exclusive(file_handle):
        msvcrt.locking(file_handle.fileno(), msvcrt.LK_LOCK, 1)
else:
    import fcntl

    def _flock_exclusive(file_handle):
        fcntl.flock(file_handle.fileno(), fcntl.LOCK_EX)

STATE_DIR = Path.home() / ".clawdmeter"
STATE_FILE = STATE_DIR / "state.json"
LOCK_FILE = STATE_DIR / "state.lock"

# Cap session retention so a forgotten session doesn't permanently
# occupy a slot in the device UI. Anything older than this gets pruned
# on the next hook fire.
SESSION_TTL_SECONDS = 15 * 60


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
        _flock_exclusive(lock)
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
            "todos": [],
        })

        # Always refresh metadata that hook payloads carry.
        if "cwd" in payload:
            session["cwd"] = payload["cwd"]
            session["project"] = _short_project(payload["cwd"])
        if "model" in payload:
            session["model"] = _short_model(payload["model"])
        session["last_active_ts"] = now

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
