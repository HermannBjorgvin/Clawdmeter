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
for cmd in curl stty inotifywait; do
    command -v "$cmd" >/dev/null || { echo "Error: $cmd is required but not installed"; exit 1; }
done
echo "  All dependencies found (curl, python3, stty)"
echo ""

# Install systemd user service with resolved path
echo "[2/3] Installing systemd user service..."
mkdir -p "$USER_SERVICE_DIR"
DAEMON_BIN="$SCRIPT_DIR/daemon/$SERVICE_NAME.sh"
sed "s|DAEMON_PATH|${DAEMON_BIN}|g" "$SERVICE_FILE" > "$USER_SERVICE_DIR/$SERVICE_NAME.service"
systemctl --user daemon-reload

# Enable (service starts automatically when device is plugged in)
echo "[3/3] Enabling service..."
systemctl --user enable "$SERVICE_NAME"

echo ""
echo "=== Done! ==="
echo ""
echo "The daemon will now start automatically when you log in"
echo "and the SC01 Plus is connected via USB."
echo ""
echo "Useful commands:"
echo "  systemctl --user status $SERVICE_NAME    # check status"
echo "  journalctl --user -u $SERVICE_NAME -f    # view logs"
echo "  systemctl --user restart $SERVICE_NAME   # restart"
echo "  systemctl --user stop $SERVICE_NAME      # stop"
echo ""
echo "To uninstall:"
echo "  systemctl --user stop $SERVICE_NAME"
echo "  systemctl --user disable $SERVICE_NAME"
echo "  rm $USER_SERVICE_DIR/$SERVICE_NAME.service"
echo "  systemctl --user daemon-reload"
