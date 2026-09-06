#!/bin/bash
# Build and flash Clawdmeter firmware on macOS.
# Usage:
#   ./flash-mac.sh <board>                       # auto-detect /dev/cu.usbmodem*
#   ./flash-mac.sh <board> /dev/cu.usbmodem1101  # explicit USB serial port
#
# <board> is the PlatformIO env name, e.g. waveshare_amoled_216 or waveshare_amoled_18.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BOARD="$1"
PORT="$2"

if [ -z "$BOARD" ]; then
    echo "Error: board env name is required."
    echo "Usage: $0 <board> [port]"
    echo "Available boards:"
    grep -E '^\[env:' "$SCRIPT_DIR/firmware/platformio.ini" | sed 's/\[env:/  /;s/\]//'
    exit 1
fi

if [ -z "$PORT" ]; then
    # Native-USB boards (S3/C6) enumerate as cu.usbmodem*; boards behind a
    # USB-UART bridge (the CYD's CH340, CP210x kits) as cu.usbserial* or
    # cu.wchusbserial*. Try the native ones first, then the bridges.
    PORT=$(ls /dev/cu.usbmodem* /dev/cu.wchusbserial* /dev/cu.usbserial* 2>/dev/null | head -1)
    if [ -z "$PORT" ]; then
        echo "Error: no USB serial device found (looked for /dev/cu.usbmodem*,"
        echo "       /dev/cu.wchusbserial*, /dev/cu.usbserial*)."
        echo "       Plug the board in. CH340-based boards may need the driver:"
        echo "       https://www.wch-ic.com/downloads/CH34XSER_MAC_ZIP.html"
        exit 1
    fi
fi

if ! command -v pio >/dev/null; then
    echo "Error: 'pio' not found. Install with:"
    echo "  brew install platformio"
    exit 1
fi

echo "=== Flashing Clawdmeter ==="
echo "Board: $BOARD"
echo "Port:  $PORT"
echo ""

cd "$SCRIPT_DIR/firmware"
pio run -e "$BOARD" -t upload --upload-port "$PORT"

echo ""
echo "=== Done ==="
echo "Monitor with: pio device monitor -p $PORT -b 115200"
