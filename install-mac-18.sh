#!/bin/bash
# macOS installer for Clawdmeter daemon (Python + bleak + launchd).
# Variant for the Waveshare ESP32-S3 1.8" AMOLED (SH8601/FT3168) board.
#
# The host-side daemon is the same regardless of which display board you
# flashed — it talks to the firmware over BLE. This script also makes sure
# PlatformIO is available so you can flash with ./flash-mac-18.sh afterwards.
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

echo "=== Clawdmeter macOS install (1.8\" board) ==="
echo ""

echo "[1/6] Checking prerequisites..."
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

echo "[2/6] Checking PlatformIO (needed to flash firmware)..."
PIO_FOUND=""
if command -v pio >/dev/null; then
    PIO_FOUND="pio"
else
    for py in python3.13 python3.12 python3.11 python3.10; do
        if command -v "$py" >/dev/null && "$py" -c "import platformio" 2>/dev/null; then
            PIO_FOUND="$py -m platformio"
            break
        fi
    done
fi

if [ -z "$PIO_FOUND" ]; then
    echo "  PlatformIO not found. Installing via pip into the user site..."
    INSTALL_PY=""
    for py in python3.13 python3.12 python3.11 python3.10; do
        if command -v "$py" >/dev/null; then
            INSTALL_PY="$py"
            break
        fi
    done
    if [ -z "$INSTALL_PY" ]; then
        echo "  Error: PlatformIO requires Python 3.10+. Install one of:"
        echo "    brew install python@3.12"
        echo "    brew install platformio"
        exit 1
    fi
    "$INSTALL_PY" -m pip install --user --upgrade platformio
    PIO_FOUND="$INSTALL_PY -m platformio"
fi
echo "  OK ($PIO_FOUND)"
echo ""

echo "[3/6] Creating Python virtualenv at daemon/.venv ..."
if [ ! -d "$VENV_DIR" ]; then
    python3 -m venv "$VENV_DIR"
fi
"$VENV_DIR/bin/pip" install --quiet --upgrade pip
"$VENV_DIR/bin/pip" install --quiet "bleak>=0.22" "httpx>=0.27"
PYTHON_BIN="$VENV_DIR/bin/python"
echo "  OK ($PYTHON_BIN)"
echo ""

echo "[4/6] Rendering launchd plist..."
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

echo "[5/6] Bluetooth permission check..."
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

echo "[6/6] Loading launchd service..."
launchctl unload "$PLIST_DST" 2>/dev/null || true
launchctl load -w "$PLIST_DST"
echo "  Loaded."
echo ""

echo "=== Done ==="
echo ""
echo "Next step — flash the 1.8\" board firmware:"
echo "  ./flash-mac-18.sh"
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
