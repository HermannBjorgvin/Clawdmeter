#!/usr/bin/env python3
"""
Claude Code hook helper for Clawdmeter.

Two responsibilities:
 1. Maintain per-session state files at /tmp/clawdmeter-sessions/<id>.json so
    the device can render a "Sessions" page with every active CC session.
 2. Drive the device's "attention" overlay (any session in waiting state).

Wire into ~/.claude/settings.json. The argv pattern is:
    clawdmeter-hook.py state <waiting|working|idle>
    clawdmeter-hook.py end

Mapping:
    SessionStart     → state idle
    UserPromptSubmit → state working
    PreToolUse       → state working
    PostToolUse      → state working
    Stop             → state idle
    Notification     → state waiting
    SessionEnd       → end

Per-session JSON shape:
    { "state": "waiting|working|idle", "cwd": "...", "msg": "...", "ts": 12345 }
"""
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

SESSIONS_DIR = Path("/tmp/clawdmeter-sessions")
SOUND_FILE   = "/System/Library/Sounds/Glass.aiff"


def safe_name(s: str) -> str:
    return re.sub(r"[^A-Za-z0-9-]", "_", s) or "unknown"


def read_event():
    try:
        return json.loads(sys.stdin.read() or "{}")
    except Exception:
        return {}


def main(argv):
    if len(argv) < 2:
        sys.exit("usage: clawdmeter-hook.py <state|end> [waiting|working|idle]")

    action = argv[1]
    state  = argv[2] if len(argv) >= 3 else None

    SESSIONS_DIR.mkdir(parents=True, exist_ok=True)

    evt = read_event()
    sid = evt.get("session_id") or f"ppid{os.getppid()}"
    cwd = evt.get("cwd") or (evt.get("workspace") or {}).get("project_dir") or ""
    msg = (evt.get("message") or "").replace("\n", " ").strip()

    f = SESSIONS_DIR / f"{safe_name(sid)}.json"

    if action == "end":
        try: f.unlink()
        except FileNotFoundError: pass
    elif action == "state":
        if state not in ("waiting", "working", "idle"):
            sys.exit(f"bad state: {state!r}")

        # Carry forward cwd from prior writes (PostToolUse etc. don't repeat it).
        prior = {}
        if f.exists():
            try: prior = json.loads(f.read_text())
            except Exception: pass

        out = {
            "state": state,
            "cwd":   cwd or prior.get("cwd", ""),
            # Only attach msg while waiting; clear it when the session moves on.
            "msg":   (msg or "Claude is waiting") if state == "waiting" else "",
            "ts":    int(time.time()),
        }
        f.write_text(json.dumps(out))

        if state == "waiting":
            subprocess.Popen(
                ["/usr/bin/afplay", "-v", "0.4", SOUND_FILE],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
    else:
        sys.exit(f"bad action: {action!r}")

    # Bump dir mtime so the daemon's mtime-watcher picks up the change.
    SESSIONS_DIR.touch()


if __name__ == "__main__":
    main(sys.argv)
