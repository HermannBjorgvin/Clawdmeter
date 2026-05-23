#!/bin/bash
# Build and flash Clawdmeter firmware on macOS.
# Usage:
#   ./flash-mac.sh                                          # default env, auto-detect port
#   ./flash-mac.sh waveshare_amoled_216                     # explicit env, auto-detect port
#   ./flash-mac.sh waveshare_amoled_206 /dev/cu.usbmodem1101 # explicit env + port
#
# Without an env, all envs in platformio.ini are built and the last one
# flashed wins — almost never what you want with multiple boards defined.
set -e

DEFAULT_ENV="waveshare_amoled_216"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ENV="${1:-$DEFAULT_ENV}"
PORT="$2"

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
echo "Monitor with: pio device monitor -e $ENV -p $PORT -b 115200"
