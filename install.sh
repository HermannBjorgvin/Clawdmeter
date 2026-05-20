#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SERVICE_NAME="claude-usage-daemon"
SERVICE_FILE="$SCRIPT_DIR/daemon/$SERVICE_NAME.service"
USER_SERVICE_DIR="$HOME/.config/systemd/user"

echo "=== Claude Usage Tracker - Install ==="
echo ""

# Check dependencies
echo "[1/3] Checking dependencies..."
for cmd in curl awk stty printf; do
    command -v "$cmd" >/dev/null || { echo "Error: $cmd is required but not installed"; exit 1; }
done
echo "  All dependencies found"
echo ""

# Install systemd user service with resolved path
echo "[2/3] Installing systemd user service..."
mkdir -p "$USER_SERVICE_DIR"
DAEMON_BIN="$SCRIPT_DIR/daemon/$SERVICE_NAME.sh"
sed "s|DAEMON_PATH|${DAEMON_BIN}|g" "$SERVICE_FILE" > "$USER_SERVICE_DIR/$SERVICE_NAME.service"
systemctl --user daemon-reload

# Enable service
echo "[3/3] Enabling service..."
systemctl --user enable "$SERVICE_NAME"

echo ""
echo "=== Done! ==="
echo ""
echo "The daemon will start automatically on login and connect to the"
echo "Clawdmeter board over USB CDC (default port: /dev/ttyACM0)."
echo ""
echo "First-time setup:"
echo "  1. Plug the Clawdmeter in via USB-C"
echo "  2. Confirm /dev/ttyACM0 appears: ls /dev/ttyACM*"
echo "  3. Add yourself to the 'dialout' (or 'uucp') group if you get a"
echo "     permission error on /dev/ttyACM0:"
echo "       sudo usermod -aG dialout \$USER  # then re-login"
echo "  4. Start the daemon: systemctl --user start $SERVICE_NAME"
echo ""
echo "Override the port:    DEVICE_PORT=/dev/ttyACM1 systemctl --user edit $SERVICE_NAME"
echo ""
echo "Useful commands:"
echo "  systemctl --user status $SERVICE_NAME    # check status"
echo "  journalctl --user -u $SERVICE_NAME -f    # view logs"
echo "  systemctl --user restart $SERVICE_NAME   # restart"
echo "  systemctl --user stop $SERVICE_NAME      # stop"
echo ""
