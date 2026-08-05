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
    case "$BOARD" in
        m5stack_fire)
            # ESP32-classic over a CH9102 UART bridge → /dev/cu.usbserial-* or
            # /dev/cu.wchusbserial* (NOT the native-USB usbmodem* the S3/C6 use).
            PORT=$(ls /dev/cu.usbserial-* /dev/cu.wchusbserial* 2>/dev/null | head -1)
            if [ -z "$PORT" ]; then
                echo "Error: no /dev/cu.usbserial-* (CH9102 UART bridge) device found."
                echo "Plug the M5Stack FIRE in via USB-C. If it still isn't seen, install"
                echo "the CH34x/CH9102 driver from https://www.wch-ic.com/downloads/CH34XSER_MAC_ZIP.html"
                exit 1
            fi
            ;;
        *)
            # S3 / C6 boards expose a native USB-JTAG serial → /dev/cu.usbmodem*
            PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
            if [ -z "$PORT" ]; then
                echo "Error: no /dev/cu.usbmodem* device found. Plug in via USB-C."
                exit 1
            fi
            ;;
    esac
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
