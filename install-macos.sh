#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DAEMON_DIR="$SCRIPT_DIR/daemon"
VENV="$DAEMON_DIR/.venv"
LABEL="com.clawdmeter.usage-daemon"
PLIST_SRC="$DAEMON_DIR/$LABEL.plist"
PLIST_DST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG_DIR="$HOME/Library/Logs"
DAEMON_BIN="$DAEMON_DIR/claude-usage-daemon.py"

echo "=== Clawdmeter macOS install ==="
echo ""

# Check dependencies
echo "[1/4] Checking dependencies..."
command -v python3 >/dev/null || { echo "Error: python3 is required"; exit 1; }
PY_VER=$(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])')
echo "  python3 $PY_VER"
echo ""

# Create venv and install dependencies
echo "[2/4] Creating venv at $VENV and installing bleak..."
python3 -m venv "$VENV"
"$VENV/bin/pip" install --quiet --upgrade pip
"$VENV/bin/pip" install --quiet -r "$DAEMON_DIR/requirements.txt"
chmod +x "$DAEMON_BIN"
echo ""

# Install LaunchAgent plist with substituted paths
echo "[3/4] Installing LaunchAgent plist..."
mkdir -p "$(dirname "$PLIST_DST")" "$LOG_DIR"
sed -e "s|PYTHON_PATH|$VENV/bin/python|g" \
    -e "s|DAEMON_PATH|$DAEMON_BIN|g" \
    -e "s|HOME_DIR|$HOME|g" \
    "$PLIST_SRC" > "$PLIST_DST"
plutil -lint "$PLIST_DST" >/dev/null
echo "  $PLIST_DST"
echo ""

# Bootstrap into launchd (replace any existing instance)
echo "[4/4] Loading LaunchAgent..."
launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$PLIST_DST"
echo ""

echo "=== Done! ==="
echo ""
echo "First-run setup:"
echo "  1. Power on the Clawdmeter."
echo "  2. Pair the device once via System Settings -> Bluetooth"
echo "     (look for 'Claude Controller')."
echo "  3. On first BLE access, macOS will prompt to allow Python to access"
echo "     Bluetooth. Accept it. If you miss the prompt, grant it later in"
echo "     System Settings -> Privacy & Security -> Bluetooth."
echo ""
echo "Useful commands:"
echo "  launchctl kickstart -k gui/$(id -u)/$LABEL   # restart"
echo "  launchctl print gui/$(id -u)/$LABEL          # status"
echo "  tail -f $LOG_DIR/claude-usage-daemon.log     # logs"
echo "  launchctl bootout gui/$(id -u)/$LABEL        # uninstall"
echo ""
