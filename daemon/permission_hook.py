#!/usr/bin/env python3
"""Claude Code PreToolUse hook — routes the approval prompt to Clawdmeter.

Registered in settings.json against a matcher (see README). Claude Code
spawns this once per matching tool call, feeds the tool-call JSON on stdin,
and waits (up to the hook's configured `timeout`) for a decision on stdout.

This script never talks to BLE itself — the usage daemon already owns the
live connection, so this just relays through its Unix socket
(`~/.config/claude-usage-monitor/permission.sock`) and waits for the reply.

Fail-safe design: every exit path below prints valid hookSpecificOutput JSON
and exits 0. On any problem — daemon not running, no device connected, no tap
within the timeout, a crash in this script — the decision is "ask", never
"deny" and never a silent non-decision. "ask" just falls back to Claude
Code's normal terminal/UI prompt, so a hardware hiccup can slow you down but
can never itself block or silently rubber-stamp a tool call.
"""
import json
import socket
import sys
from pathlib import Path

# Kept in sync with claude_usage_daemon.PERM_SOCK_FILE. Not imported from it
# to avoid dragging in bleak/httpx (and their import time) for every single
# tool call — this script needs to be fast.
PERM_SOCK_FILE = Path.home() / ".config" / "claude-usage-monitor" / "permission.sock"

# Give the daemon's own wait-for-tap budget (PERM_REQ_DEFAULT_TIMEOUT_S = 45s,
# or whatever the request below asks for) room to actually finish and reply,
# plus slack for socket/connect overhead — and keep it comfortably under this
# hook's own settings.json `timeout`, which must be configured larger still
# (see README). If our own budget runs out first, we still answer "ask"
# ourselves rather than letting Claude Code's hook-level timeout do it, since
# that path is documented to fail *open* (silently proceeds), not neutral.
SOCKET_TIMEOUT_S = 50
REQUESTED_TIMEOUT_S = 45


def summarize(tool_name: str, tool_input: dict) -> str:
    """A short, human-readable line for the device's small screen."""
    if tool_name == "Bash":
        text = tool_input.get("command", "") or tool_input.get("description", "")
    elif tool_name in ("Write", "Edit", "NotebookEdit"):
        text = tool_input.get("file_path", "")
    elif tool_name == "Read":
        text = tool_input.get("file_path", "")
    else:
        # MCP tools and anything else: best-effort, first field's value.
        text = next(iter(tool_input.values()), "") if tool_input else ""
        text = str(text)
    text = " ".join(text.split())  # collapse newlines/whitespace
    return text[:120]


def ask(reason: str) -> dict:
    return {
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "ask",
            "permissionDecisionReason": reason,
        }
    }


def decide(decision: str, reason: str) -> dict:
    # "always" is treated as a one-time allow for now — it does not yet
    # persist a standing rule. See README's "Known limitation" note.
    mapped = "allow" if decision in ("allow", "always") else "deny" if decision == "deny" else "ask"
    out = {
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": mapped,
            "permissionDecisionReason": reason,
        }
    }
    if decision == "always":
        out["hookSpecificOutput"]["systemMessage"] = (
            "Tapped 'Always Allow' on Clawdmeter — allowed for this call only; "
            "it does not yet persist as a standing permission rule."
        )
    return out


def main() -> None:
    try:
        req = json.load(sys.stdin)
        tool_name = req.get("tool_name", "Tool")
        tool_input = req.get("tool_input", {}) or {}
    except (json.JSONDecodeError, UnicodeDecodeError) as e:
        print(json.dumps(ask(f"hook could not parse stdin: {e}")))
        return

    summary = summarize(tool_name, tool_input)

    if not PERM_SOCK_FILE.exists():
        print(json.dumps(ask("Clawdmeter daemon not running (no permission socket)")))
        return

    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(SOCKET_TIMEOUT_S)
            sock.connect(str(PERM_SOCK_FILE))
            request = json.dumps({
                "tool": tool_name,
                "summary": summary,
                "timeout_s": REQUESTED_TIMEOUT_S,
            }) + "\n"
            sock.sendall(request.encode())

            chunks = []
            while True:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                chunks.append(chunk)
                if b"\n" in chunk:
                    break
            line = b"".join(chunks).split(b"\n", 1)[0]
    except (OSError, socket.timeout) as e:
        print(json.dumps(ask(f"could not reach Clawdmeter daemon: {e}")))
        return

    try:
        resp = json.loads(line.decode())
        decision = resp.get("decision", "ask")
        reason = resp.get("reason") or f"Clawdmeter: {decision}"
    except (json.JSONDecodeError, UnicodeDecodeError) as e:
        print(json.dumps(ask(f"malformed daemon reply: {e}")))
        return

    print(json.dumps(decide(decision, reason)))


if __name__ == "__main__":
    try:
        main()
    except Exception as e:  # noqa: BLE001 — absolute last resort, must still emit valid JSON
        print(json.dumps(ask(f"hook crashed: {e}")))
