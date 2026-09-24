#!/bin/bash
# Build and flash Clawdmeter firmware on Linux.
# Usage:
#   ./flash.sh <board>                  # default port /dev/ttyACM0
#   ./flash.sh <board> /dev/ttyACM1     # explicit USB serial port
#
# <board> is the PlatformIO env name, e.g. waveshare_amoled_216 or waveshare_amoled_18.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BOARD="$1"
# Native-USB boards (S3/C6) enumerate as /dev/ttyACM*; boards behind a
# USB-UART bridge (the CYD's CH340) as /dev/ttyUSB*.
PORT="${2:-$(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1)}"
PORT="${PORT:-/dev/ttyACM0}"

if [ -z "$BOARD" ]; then
    echo "Error: board env name is required."
    echo "Usage: $0 <board> [port]"
    echo "Available boards:"
    grep -E '^\[env:' "$SCRIPT_DIR/firmware/platformio.ini" | sed 's/\[env:/  /;s/\]//'
    exit 1
fi

echo "=== Flashing Clawdmeter ==="
echo "Board: $BOARD"
echo "Port:  $PORT"
echo ""

cd "$SCRIPT_DIR/firmware"
~/.platformio/penv/bin/pio run -e "$BOARD" -t upload --upload-port "$PORT"

echo ""
echo "=== Done! ==="
