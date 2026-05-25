#!/bin/bash
# Switch the Clawdmeter host daemon usage provider and reload launchd.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONFIG="$SCRIPT_DIR/daemon/config.toml"
SERVICE_LABEL="com.user.claude-usage-daemon"
PLIST="$HOME/Library/LaunchAgents/$SERVICE_LABEL.plist"
PROVIDER="${1:-}"

if [[ "$PROVIDER" != "claude" && "$PROVIDER" != "codex" && "$PROVIDER" != "both" ]]; then
    echo "Usage: $0 <claude|codex|both>"
    exit 1
fi

mkdir -p "$(dirname "$CONFIG")"
if [[ -f "$CONFIG" ]] && grep -Eq '^[[:space:]]*provider[[:space:]]*=' "$CONFIG"; then
    tmp="$(mktemp)"
    sed -E "s|^[[:space:]]*provider[[:space:]]*=.*|provider = \"$PROVIDER\"|" "$CONFIG" > "$tmp"
    mv "$tmp" "$CONFIG"
else
    printf 'provider = "%s"\n' "$PROVIDER" >> "$CONFIG"
fi

echo "Provider set to: $PROVIDER"

if [[ ! -f "$PLIST" ]]; then
    echo "LaunchAgent is not installed yet."
    echo "Run ./install-mac.sh, then re-run this switch command."
    exit 0
fi

launchctl unload "$PLIST" 2>/dev/null || true
launchctl load -w "$PLIST"

echo "LaunchAgent reloaded: $SERVICE_LABEL"
echo "The next usage poll should appear in the log within about 60 seconds:"
echo "  tail -F ~/Library/Logs/claude-usage-daemon.out.log"
