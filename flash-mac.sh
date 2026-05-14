#!/bin/bash
# Build and flash Clawdmeter firmware on macOS.
# Usage:
#   ./flash-mac.sh                       # auto-detect port, build env waveshare_amoled_18
#   ./flash-mac.sh -e waveshare_amoled_216
#   ./flash-mac.sh /dev/cu.usbmodem1101
#   ./flash-mac.sh -e <env> /dev/cu.usbmodem1101
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ENV="waveshare_amoled_18"
PORT=""

while [ $# -gt 0 ]; do
    case "$1" in
        -e|--env) ENV="$2"; shift 2 ;;
        /dev/*)   PORT="$1"; shift ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

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
echo "Env:  $ENV"
echo "Port: $PORT"
echo ""

cd "$SCRIPT_DIR/firmware"
pio run -e "$ENV" -t upload --upload-port "$PORT"

echo ""
echo "=== Done ==="
echo "Monitor with: pio device monitor -p $PORT -b 115200"
