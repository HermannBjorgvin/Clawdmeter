#!/bin/bash
# macOS installer for Clawdmeter daemon (USB CDC, Python + pyserial + launchd).
# Mirrors install.sh; uses LaunchAgents instead of systemd user units.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SERVICE_LABEL="com.user.claude-usage-daemon"
PLIST_SRC="$SCRIPT_DIR/daemon/$SERVICE_LABEL.plist"
PLIST_DST="$HOME/Library/LaunchAgents/$SERVICE_LABEL.plist"
VENV_DIR="$SCRIPT_DIR/daemon/.venv"
DAEMON_PY="$SCRIPT_DIR/daemon/claude_usage_daemon.py"
LOG_DIR="$HOME/Library/Logs"
LOG_OUT="$LOG_DIR/claude-usage-daemon.out.log"
LOG_ERR="$LOG_DIR/claude-usage-daemon.err.log"

echo "=== Clawdmeter macOS install (USB CDC) ==="
echo ""

echo "[1/4] Checking prerequisites..."
for cmd in python3 curl; do
    command -v "$cmd" >/dev/null || { echo "Error: $cmd is required"; exit 1; }
done
if [ ! -f "$HOME/.claude/.credentials.json" ]; then
    echo "Note: ~/.claude/.credentials.json not found — the daemon reads"
    echo "      the token from the Keychain on macOS (service 'Claude Code-credentials')."
    echo "      Sign in via Claude Code first if you haven't."
fi
echo "  OK"
echo ""

echo "[2/4] Creating Python virtualenv at daemon/.venv ..."
if [ ! -d "$VENV_DIR" ]; then
    python3 -m venv "$VENV_DIR"
fi
"$VENV_DIR/bin/pip" install --quiet --upgrade pip
"$VENV_DIR/bin/pip" install --quiet "pyserial>=3.5" "httpx>=0.27"
PYTHON_BIN="$VENV_DIR/bin/python"
echo "  OK ($PYTHON_BIN)"
echo ""

echo "[3/4] Rendering launchd plist..."
mkdir -p "$HOME/Library/LaunchAgents" "$LOG_DIR"
sed \
    -e "s|__PYTHON_BIN__|${PYTHON_BIN}|g" \
    -e "s|__DAEMON_PATH__|${DAEMON_PY}|g" \
    -e "s|__REPO_DIR__|${SCRIPT_DIR}|g" \
    -e "s|__LOG_OUT__|${LOG_OUT}|g" \
    -e "s|__LOG_ERR__|${LOG_ERR}|g" \
    -e "s|__HOME__|${HOME}|g" \
    "$PLIST_SRC" > "$PLIST_DST"
echo "  Installed: $PLIST_DST"
echo ""

echo "[4/4] Loading launchd service..."
launchctl unload "$PLIST_DST" 2>/dev/null || true
launchctl load -w "$PLIST_DST"
echo "  Loaded."
echo ""

echo "=== Done ==="
echo ""
echo "First-time setup (after firmware is flashed):"
echo "  1. Plug the Clawdmeter into your Mac via USB-C."
echo "  2. Confirm the port appears: ls /dev/cu.usbmodem*"
echo "  3. The daemon should pick it up within a few seconds — no pairing needed."
echo ""
echo "Override the port (rarely needed):"
echo "  DEVICE_PORT=/dev/cu.usbmodem1101 in the plist's EnvironmentVariables."
echo ""
echo "Useful commands:"
echo "  launchctl list | grep claude-usage     # check it's running"
echo "  tail -F $LOG_OUT                       # live logs"
echo "  launchctl unload $PLIST_DST            # stop"
echo "  launchctl load -w $PLIST_DST           # start"
