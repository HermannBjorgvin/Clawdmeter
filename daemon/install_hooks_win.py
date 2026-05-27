"""Register Clawdmeter Claude Code hooks in %USERPROFILE%\\.claude\\settings.json.

Windows equivalent of the inline Python block in install-mac.sh. Idempotent:
re-running won't add duplicates. Atomic: writes to a temp file then renames.
Always makes a timestamped backup before touching anything.
"""
import json
import os
import shutil
import sys
import time
from pathlib import Path


EVENTS = ["PreToolUse", "PostToolUse", "Stop", "UserPromptSubmit", "SessionStart"]


def main() -> int:
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <python_exe> <hook_script>", file=sys.stderr)
        return 2

    python_exe = sys.argv[1]
    hook_script = sys.argv[2]
    hook_cmd = f'"{python_exe}" "{hook_script}"'

    settings_path = Path.home() / ".claude" / "settings.json"
    settings_path.parent.mkdir(parents=True, exist_ok=True)

    # Backup before any write.
    if settings_path.exists():
        ts = time.strftime("%Y%m%d-%H%M%S")
        backup = settings_path.with_suffix(f".json.bak.{ts}")
        shutil.copy2(settings_path, backup)
        print(f"  Backup: {backup}")

    try:
        data = json.loads(settings_path.read_text())
        if not isinstance(data, dict):
            data = {}
    except (FileNotFoundError, json.JSONDecodeError):
        data = {}

    hooks = data.setdefault("hooks", {})
    if not isinstance(hooks, dict):
        hooks = {}
        data["hooks"] = hooks

    added = 0
    skipped = 0
    for event in EVENTS:
        rules = hooks.setdefault(event, [])
        if not isinstance(rules, list):
            rules = []
        already = any(
            any(
                isinstance(h, dict) and h.get("command") == hook_cmd
                for h in (r.get("hooks", []) if isinstance(r, dict) else [])
            )
            for r in rules
        )
        if already:
            skipped += 1
            continue
        rules.append({
            "matcher": "",
            "hooks": [{"type": "command", "command": hook_cmd}],
        })
        hooks[event] = rules
        added += 1

    tmp = str(settings_path) + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
    os.replace(tmp, str(settings_path))

    print(f"  Wrote: {settings_path}")
    print(f"  Added: {added} hook binding(s); skipped {skipped} already-registered.")
    print(f"  Hook command: {hook_cmd}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
