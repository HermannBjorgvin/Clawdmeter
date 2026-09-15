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

"Always Allow" persistence: tapping Always writes a standing rule into the
CURRENT PROJECT's `.claude/settings.local.json` `permissions.allow` — the
same file and array Claude Code's own "don't ask again" writes to, using the
same narrow, per-subcommand/per-file scoping it uses (see rules_to_persist).
This hook then self-checks that file on every future call for this project
and answers "allow" immediately, without bothering the device, for anything
already covered. This does NOT rely on Claude Code's own permission engine
noticing the file mid-session (undocumented whether it does) — the checking
happens entirely in this script, which reads the file fresh on every
invocation, so it's correct from the very next tool call onward regardless.
"""
import json
import os
import re
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

# Rule-persistence scoping: PROJECT-local only (matches Claude Code's own
# "don't ask again" behavior) — a per-project .claude/settings.local.json,
# never the global ~/.claude/settings.json. Tapping Always in one project
# never grants anything in another.
PATH_TOOLS = ("Write", "Edit", "NotebookEdit")

# Same delimiter set Claude Code's own Bash permission matching splits a
# compound command on (longest operators first so "&&" isn't half-matched
# by the "&" alternative). Best-effort, not a real shell parser — a delimiter
# character sitting inside quotes can still mis-split, same caveat any
# regex-based approach has.
_BASH_SPLIT_RE = re.compile(r"&&|\|\||;|\|&|\||&|\n")


def split_subcommands(command: str) -> list[str]:
    return [p.strip() for p in _BASH_SPLIT_RE.split(command) if p.strip()]


def path_field(tool_input: dict) -> str:
    return tool_input.get("file_path") or tool_input.get("notebook_path") or ""


def summarize(tool_name: str, tool_input: dict) -> str:
    """A short, human-readable line for the device's small screen."""
    if tool_name == "Bash":
        text = tool_input.get("command", "") or tool_input.get("description", "")
    elif tool_name in PATH_TOOLS or tool_name == "Read":
        text = path_field(tool_input)
    else:
        # MCP tools and anything else: best-effort, first field's value.
        text = next(iter(tool_input.values()), "") if tool_input else ""
        text = str(text)
    text = " ".join(text.split())  # collapse newlines/whitespace
    return text[:120]


def find_project_root(cwd: str) -> Path:
    """Walk up from cwd looking for a .git dir; fall back to cwd itself.

    Matches the documented native behavior: project-local rules live at the
    repo root in a git project, or the working directory outside one.
    """
    start = Path(cwd) if cwd else Path.cwd()
    cur = start
    while True:
        if (cur / ".git").exists():
            return cur
        if cur.parent == cur:
            return start
        cur = cur.parent


def settings_local_path(project_root: Path) -> Path:
    return project_root / ".claude" / "settings.local.json"


def load_allow_rules(settings_path: Path) -> set:
    if not settings_path.exists():
        return set()
    try:
        data = json.loads(settings_path.read_text())
    except (json.JSONDecodeError, OSError, UnicodeDecodeError):
        return set()
    return set(data.get("permissions", {}).get("allow", []))


def file_path_rule_candidates(tool_name: str, file_path: str, project_root: Path) -> list:
    """Every rule-string form that could plausibly cover this file, preferred first.

    Project-relative form matches what a human would normally write; the
    absolute `TOOL(//path)` form is the fallback for a file outside the
    project root (e.g. a config file under $HOME).
    """
    candidates = []
    try:
        rel = os.path.relpath(file_path, project_root)
        if not rel.startswith(".."):
            candidates.append(f"{tool_name}({rel})")
    except ValueError:
        pass  # different drive on Windows, etc. — fall through to absolute form
    candidates.append(f"{tool_name}(//{file_path.lstrip('/')})")
    return candidates


def already_allowed(tool_name: str, tool_input: dict, rules: set, project_root: Path) -> bool:
    """True if a persisted rule already covers this exact call, no tap needed."""
    if tool_name in rules or f"{tool_name}(*)" in rules:
        return True
    if tool_name == "Bash":
        subs = split_subcommands(tool_input.get("command", ""))
        return bool(subs) and all(f"Bash({s})" in rules for s in subs)
    if tool_name in PATH_TOOLS:
        file_path = path_field(tool_input)
        if not file_path:
            return False
        return any(c in rules for c in file_path_rule_candidates(tool_name, file_path, project_root))
    return False


def rules_to_persist(tool_name: str, tool_input: dict, project_root: Path) -> list:
    """Narrow, native-matching rule(s) for what was just tapped Always on.

    Deliberately as narrow as Claude Code's own "don't ask again": one exact
    rule per Bash subcommand (not a wildcard prefix), one file per Write/Edit/
    NotebookEdit. A tap only ever grants what it visibly just approved.
    """
    if tool_name == "Bash":
        subs = split_subcommands(tool_input.get("command", ""))
        return [f"Bash({s})" for s in subs][:5]  # matches Claude Code's own per-command cap
    if tool_name in PATH_TOOLS:
        file_path = path_field(tool_input)
        if not file_path:
            return []
        return file_path_rule_candidates(tool_name, file_path, project_root)[:1]
    return []


def _ensure_gitignored(settings_path: Path) -> None:
    """Best-effort — never let a .gitignore hiccup affect the actual decision."""
    project_root = settings_path.parent.parent
    if not (project_root / ".git").exists():
        return
    entry = ".claude/settings.local.json"
    gitignore = project_root / ".gitignore"
    try:
        if gitignore.exists():
            if entry in gitignore.read_text().splitlines():
                return
            with gitignore.open("a") as f:
                if gitignore.stat().st_size and not gitignore.read_text().endswith("\n"):
                    f.write("\n")
                f.write(entry + "\n")
        else:
            gitignore.write_text(entry + "\n")
    except OSError:
        pass


def persist_allow_rules(settings_path: Path, new_rules: list) -> None:
    if not new_rules:
        return
    data = {}
    if settings_path.exists():
        try:
            data = json.loads(settings_path.read_text())
        except (json.JSONDecodeError, OSError, UnicodeDecodeError):
            data = {}
    perms = data.setdefault("permissions", {})
    allow = perms.setdefault("allow", [])
    changed = False
    for r in new_rules:
        if r not in allow:
            allow.append(r)
            changed = True
    if not changed:
        return
    settings_path.parent.mkdir(parents=True, exist_ok=True)
    settings_path.write_text(json.dumps(data, indent=2) + "\n")
    _ensure_gitignored(settings_path)


def ask(reason: str) -> dict:
    return {
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "ask",
            "permissionDecisionReason": reason,
        }
    }


def allow(reason: str) -> dict:
    return {
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "allow",
            "permissionDecisionReason": reason,
        }
    }


def decide(decision: str, reason: str, persisted: list) -> dict:
    mapped = "allow" if decision in ("allow", "always") else "deny" if decision == "deny" else "ask"
    out = {
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": mapped,
            "permissionDecisionReason": reason,
        }
    }
    if decision == "always":
        if persisted:
            rule_list = ", ".join(f"`{r}`" for r in persisted)
            out["hookSpecificOutput"]["systemMessage"] = (
                f"Tapped 'Always Allow' on Clawdmeter — saved {rule_list} to this "
                "project's .claude/settings.local.json. Won't prompt again for "
                "an exact repeat of this call in this project."
            )
        else:
            out["hookSpecificOutput"]["systemMessage"] = (
                "Tapped 'Always Allow' on Clawdmeter — allowed for this call only; "
                "couldn't determine a rule to persist for this tool."
            )
    return out


def main() -> None:
    try:
        req = json.load(sys.stdin)
        tool_name = req.get("tool_name", "Tool")
        tool_input = req.get("tool_input", {}) or {}
        cwd = req.get("cwd", "")
    except (json.JSONDecodeError, UnicodeDecodeError) as e:
        print(json.dumps(ask(f"hook could not parse stdin: {e}")))
        return

    project_root = find_project_root(cwd)
    settings_path = settings_local_path(project_root)

    # Self-checked short-circuit: a prior "Always" already covers this exact
    # call. Skip the device entirely — this is what makes persistence actually
    # useful (repeats stop prompting), rather than every call still going out
    # to the device for a decision it already knows.
    existing_rules = load_allow_rules(settings_path)
    if already_allowed(tool_name, tool_input, existing_rules, project_root):
        print(json.dumps(allow("Already allowed by a persisted Clawdmeter rule "
                                f"in {settings_path}")))
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

    persisted = []
    if decision == "always":
        try:
            persisted = rules_to_persist(tool_name, tool_input, project_root)
            persist_allow_rules(settings_path, persisted)
        except OSError as e:
            # Persistence failed — still honor the tap for this call, just
            # without a systemMessage claiming it was saved.
            print(f"permission_hook: failed to persist rule: {e}", file=sys.stderr)
            persisted = []

    print(json.dumps(decide(decision, reason, persisted)))


if __name__ == "__main__":
    try:
        main()
    except Exception as e:  # noqa: BLE001 — absolute last resort, must still emit valid JSON
        print(json.dumps(ask(f"hook crashed: {e}")))
