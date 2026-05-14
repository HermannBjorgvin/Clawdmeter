#!/bin/bash
# macOS installer for Clawdmeter daemon (Python + bleak + launchd).
# Mirrors install.sh but uses LaunchAgents instead of systemd user units.
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

echo "=== Clawdmeter macOS install ==="
echo ""

echo "[1/5] Checking prerequisites..."
for cmd in python3 curl; do
    command -v "$cmd" >/dev/null || { echo "Error: $cmd is required"; exit 1; }
done
if [ ! -f "$HOME/.claude/.credentials.json" ]; then
    echo "Warning: ~/.claude/.credentials.json not found."
    echo "  Sign in via Claude Code first, then re-run this installer."
    echo "  Continuing anyway — the daemon will retry on each poll."
fi
echo "  OK"
echo ""

echo "[2/5] Creating Python virtualenv at daemon/.venv ..."
if [ ! -d "$VENV_DIR" ]; then
    python3 -m venv "$VENV_DIR"
fi
"$VENV_DIR/bin/pip" install --quiet --upgrade pip
"$VENV_DIR/bin/pip" install --quiet "bleak>=0.22" "httpx>=0.27"
PYTHON_BIN="$VENV_DIR/bin/python"
echo "  OK ($PYTHON_BIN)"
echo ""

echo "[3/5] Rendering launchd plist..."
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

echo "[4/5] Bluetooth permission check..."
echo "  On first run the daemon will trigger a Bluetooth permission prompt."
echo "  macOS only prompts for foreground processes — so we'll run it"
echo "  interactively once below. Press Ctrl+C after you see 'Scanning...'"
echo "  and grant permission when prompted. Then re-run this installer"
echo "  (or just continue) to enable launchd autostart."
echo ""
read -r -p "Run a permission-priming scan now? [Y/n] " ans
if [[ ! "$ans" =~ ^[Nn]$ ]]; then
    "$PYTHON_BIN" "$DAEMON_PY" || true
fi
echo ""

echo "[5/5] Loading launchd service..."
launchctl unload "$PLIST_DST" 2>/dev/null || true
launchctl load -w "$PLIST_DST"
echo "  Loaded."
echo ""

echo "=== Done ==="
echo ""
echo "First-time Bluetooth pairing (after firmware is flashed):"
echo "  1. Power on the device."
echo "  2. Open System Settings → Bluetooth."
echo "  3. Click 'Connect' next to 'Claude Controller'."
echo "  4. The daemon will discover it within ~30 s and start polling."
echo ""
echo "Useful commands:"
echo "  launchctl list | grep claude-usage     # check it's running"
echo "  tail -F $LOG_OUT                       # live logs"
echo "  launchctl unload $PLIST_DST            # stop"
echo "  launchctl load -w $PLIST_DST           # start"
