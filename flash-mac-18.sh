#!/bin/bash
# Build and flash Clawdmeter firmware on macOS for the
# Waveshare ESP32-S3 1.8" AMOLED (SH8601/FT3168, 368x448) board.
#
# Usage:
#   ./flash-mac-18.sh                       # auto-detect /dev/cu.usbmodem*
#   ./flash-mac-18.sh /dev/cu.usbmodem1101  # explicit USB serial port
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PORT="$1"
ENV_NAME="waveshare_amoled_18"

if [ -z "$PORT" ]; then
    PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
    if [ -z "$PORT" ]; then
        echo "Error: no /dev/cu.usbmodem* device found. Plug in via USB-C."
        exit 1
    fi
fi

# Resolve a working pio command. Prefer 'pio' on PATH; otherwise fall back to
# a Python 3.10+ install of the platformio package (pio's CLI requires it).
PIO_CMD=""
if command -v pio >/dev/null; then
    PIO_CMD="pio"
else
    for py in python3.13 python3.12 python3.11 python3.10; do
        if command -v "$py" >/dev/null; then
            if "$py" -c "import platformio" 2>/dev/null; then
                PIO_CMD="$py -m platformio"
                break
            fi
        fi
    done
fi

if [ -z "$PIO_CMD" ]; then
    echo "Error: PlatformIO not found. Install with one of:"
    echo "  brew install platformio"
    echo "  python3 -m pip install --user platformio    # needs Python 3.10+"
    exit 1
fi

echo "=== Flashing Clawdmeter (1.8\" board) ==="
echo "Port: $PORT"
echo "Env:  $ENV_NAME"
echo "Tool: $PIO_CMD"
echo ""

cd "$SCRIPT_DIR/firmware"
$PIO_CMD run -e "$ENV_NAME" -t upload --upload-port "$PORT"

echo ""
echo "=== Done ==="
echo "Monitor with: $PIO_CMD device monitor -e $ENV_NAME -p $PORT -b 115200"
