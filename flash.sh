#!/bin/bash
# Build and flash firmware to the SC01 Plus.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PORT="${1:-/dev/ttyACM0}"

echo "=== Flashing Claude Usage Tracker ==="
echo "Port: $PORT"
echo ""

cd "$SCRIPT_DIR/firmware"
~/.platformio/penv/bin/pio run -e waveshare_amoled_216 -t upload --upload-port "$PORT"

echo ""
echo "=== Done! ==="
