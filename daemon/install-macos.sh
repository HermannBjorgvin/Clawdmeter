#!/bin/bash
# Clawdmeter — macOS daemon installer.
#
# Sets up a Python venv with bleak, wires Claude Code hooks into
# ~/.claude/settings.json, and (optionally) installs a launchd agent so the
# daemon starts automatically at login.
#
# Idempotent: safe to re-run. Existing settings.json hooks are preserved
# unless they point at a stale path; matching clawdmeter-hook.py entries
# are de-duplicated.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DAEMON_DIR="$REPO_ROOT/daemon"
HOOK="$DAEMON_DIR/clawdmeter-hook.py"
DAEMON="$DAEMON_DIR/claude-usage-daemon-macos.py"
VENV="$HOME/.clawdmeter-venv"
SETTINGS="$HOME/.claude/settings.json"
PLIST_DIR="$HOME/Library/LaunchAgents"
PLIST="$PLIST_DIR/com.clawdmeter.daemon.plist"
PY="$VENV/bin/python3"

bold() { printf '\033[1m%s\033[0m\n' "$1"; }
warn() { printf '\033[33mwarn:\033[0m %s\n' "$1"; }
ok()   { printf '\033[32m  ok:\033[0m %s\n' "$1"; }

# ---- 1. Sanity ----
bold "==> Checking prerequisites"
[ -x /usr/bin/python3 ]    || { echo "macOS python3 missing"; exit 1; }
[ -x /usr/bin/security ]   || { echo "macOS security CLI missing"; exit 1; }
security find-generic-password -s "Claude Code-credentials" >/dev/null 2>&1 \
    || warn "Claude Code-credentials not in Keychain — install Claude Code and log in before running the daemon"
ok "host looks good"

# ---- 2. Python venv ----
bold "==> Setting up venv at $VENV"
if [ ! -d "$VENV" ]; then
    /usr/bin/python3 -m venv "$VENV"
fi
"$PY" -m pip install --quiet --upgrade pip
"$PY" -m pip install --quiet bleak
ok "venv ready with bleak"

# ---- 3. Hook wiring ----
bold "==> Wiring Claude Code hooks into $SETTINGS"
mkdir -p "$HOME/.claude"
if [ ! -f "$SETTINGS" ]; then
    echo '{}' > "$SETTINGS"
fi

"$PY" - "$SETTINGS" "$HOOK" <<'PY'
import json, sys, os, copy
settings_path, hook = sys.argv[1], sys.argv[2]

WIRING = {
    "SessionStart":     ("state", "idle"),
    "UserPromptSubmit": ("state", "working"),
    "PreToolUse":       ("state", "working"),
    "PostToolUse":      ("state", "working"),
    "Stop":             ("state", "idle"),
    "Notification":     ("state", "waiting"),
    "SessionEnd":       ("end",   None),
}

def cmd_for(action, arg):
    return f"{hook} {action}" + (f" {arg}" if arg else "")

with open(settings_path) as f:
    cfg = json.load(f)

cfg.setdefault("hooks", {})
for evt, (action, arg) in WIRING.items():
    target_cmd = cmd_for(action, arg)
    matchers = cfg["hooks"].setdefault(evt, [])
    # Find or create a matcher entry. Most CC events accept matcher "*".
    if matchers:
        entry = matchers[0]
        entry.setdefault("hooks", [])
    else:
        entry = {"matcher": "*", "hooks": []}
        matchers.append(entry)
    # Drop any existing clawdmeter hook entries (path may have moved).
    entry["hooks"] = [h for h in entry["hooks"]
                      if not (isinstance(h, dict) and "clawdmeter-hook" in str(h.get("command", "")))]
    entry["hooks"].append({"type": "command", "command": target_cmd})

with open(settings_path, "w") as f:
    json.dump(cfg, f, indent=2)
    f.write("\n")
print("  ok: hooks wired")
PY

# ---- 4. LaunchAgent (optional, asked once) ----
bold "==> Optional: install launchd agent so the daemon starts at login?"
read -r -p "    Install LaunchAgent at $PLIST ? [y/N] " ans
if [[ "${ans:-N}" =~ ^[Yy]$ ]]; then
    mkdir -p "$PLIST_DIR"
    cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>           <string>com.clawdmeter.daemon</string>
  <key>ProgramArguments</key>
  <array>
    <string>$PY</string>
    <string>$DAEMON</string>
  </array>
  <key>RunAtLoad</key>       <true/>
  <key>KeepAlive</key>       <true/>
  <key>StandardOutPath</key> <string>$HOME/Library/Logs/clawdmeter.log</string>
  <key>StandardErrorPath</key><string>$HOME/Library/Logs/clawdmeter.log</string>
</dict>
</plist>
EOF
    launchctl unload "$PLIST" 2>/dev/null || true
    launchctl load   "$PLIST"
    ok "agent loaded; logs at ~/Library/Logs/clawdmeter.log"
else
    echo "    skipped — run the daemon manually with:"
    echo "      $PY $DAEMON"
fi

bold "==> Done"
echo "Next steps:"
echo "  • If you haven't flashed firmware yet:"
echo "      brew install platformio"
echo "      pio run -d firmware -t upload --upload-port /dev/cu.usbmodemXXX"
echo "  • On first connection, macOS will prompt for Bluetooth + Keychain access — approve both."
