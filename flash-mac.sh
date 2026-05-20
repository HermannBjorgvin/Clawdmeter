#!/bin/bash
# Build and flash Clawdmeter firmware on macOS.
# Usage:
#   ./flash-mac.sh                       # auto-detect /dev/cu.usbmodem*
#   ./flash-mac.sh /dev/cu.usbmodem1101  # explicit USB serial port
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PORT="$1"

if [ -z "$PORT" ]; then
    PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
    if [ -z "$PORT" ]; then
        echo "Error: no /dev/cu.usbmodem* device found. Plug in via USB-C."
        exit 1
    fi
fi

if ! command -v pio >/dev/null; then
    echo "Error: 'pio' not found. Install with:"
    echo "  brew install platformio"
    exit 1
fi

echo "=== Flashing Clawdmeter ==="
echo "Port: $PORT"
echo ""

cd "$SCRIPT_DIR/firmware"
pio run -e waveshare_amoled_216 -t upload --upload-port "$PORT"

echo ""
echo "=== Done ==="
echo "Monitor with: pio device monitor -p $PORT -b 115200"
