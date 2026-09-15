#!/bin/bash
# Take a screenshot from the Waveshare AMOLED display via LVGL snapshot.
# Usage: ./screenshot.sh [output.png] [port]
# Default port: /dev/cu.usbmodem101 on macOS, /dev/ttyACM0 on Linux.

OUTPUT="${1:-screenshot.png}"
if [ -z "$2" ]; then
    case "$(uname -s)" in
        Darwin) PORT="/dev/cu.usbmodem101" ;;
        *)      PORT="/dev/ttyACM0" ;;
    esac
else
    PORT="$2"
fi

# Use pio's bundled python if pyserial isn't on the system python.
PY="python3"
if ! python3 -c "import serial" 2>/dev/null; then
    if [ -x "$HOME/.platformio/penv/bin/python" ]; then
        PY="$HOME/.platformio/penv/bin/python"
    fi
fi

TMPRAW=$(mktemp /tmp/screenshot_XXXXXX.raw)
TMPDIMS=$(mktemp /tmp/screenshot_XXXXXX.dims)
trap "rm -f '$TMPRAW' '$TMPDIMS'" EXIT

echo "Taking screenshot from $PORT..."

"$PY" - "$PORT" "$TMPRAW" "$TMPDIMS" << 'PYEOF'
import serial, sys

# The device has no PSRAM, so send_screenshot() (firmware/src/main.cpp)
# can't buffer a full frame — it streams each flushed tile as it's drawn:
#   A <x1> <y1> <x2> <y2>\n<(x2-x1+1)*(y2-y1+1)*2 raw RGB565LE bytes>\n
# repeated until SCREENSHOT_END. Reassemble tiles into one w*h*2 buffer
# rather than reading raw_size as one contiguous blob (that interleaves the
# "A ..." headers into the pixel stream and corrupts the image).

port_path, raw_path, dims_path = sys.argv[1], sys.argv[2], sys.argv[3]

def read_line(p):
    return p.readline().decode("utf-8", errors="replace").strip()

def read_exact(p, n):
    data = bytearray()
    while len(data) < n:
        chunk = p.read(n - len(data))
        if not chunk:
            print(f"Timeout: got {len(data)} of {n} bytes", file=sys.stderr)
            sys.exit(1)
        data += chunk
    return bytes(data)

port = serial.Serial(port_path, 115200, timeout=10)
port.reset_input_buffer()
port.write(b"screenshot\n")
port.flush()

while True:
    line = read_line(port)
    if line.startswith("SCREENSHOT_START"):
        parts = line.split()
        w, h, raw_size = int(parts[1]), int(parts[2]), int(parts[3])
        break
    if line == "SCREENSHOT_ERR":
        print("Device reported screenshot error", file=sys.stderr)
        sys.exit(1)

framebuf = bytearray(w * h * 2)
total_px_bytes = 0

while total_px_bytes < raw_size:
    line = read_line(port)
    if line == "SCREENSHOT_END":
        break
    if not line.startswith("A "):
        continue  # stray/blank line between tiles
    _, x1s, y1s, x2s, y2s = line.split()
    x1, y1, x2, y2 = int(x1s), int(y1s), int(x2s), int(y2s)
    tile_w, tile_h = x2 - x1 + 1, y2 - y1 + 1
    tile_bytes = read_exact(port, tile_w * tile_h * 2)
    port.read(1)  # trailing '\n' after the binary chunk

    row_bytes = tile_w * 2
    for row in range(tile_h):
        dst_off = ((y1 + row) * w + x1) * 2
        src_off = row * row_bytes
        framebuf[dst_off:dst_off + row_bytes] = tile_bytes[src_off:src_off + row_bytes]
    total_px_bytes += len(tile_bytes)

for _ in range(10):
    line = read_line(port)
    if line == "SCREENSHOT_END" or not line:
        break

with open(raw_path, "wb") as f:
    f.write(framebuf)
with open(dims_path, "w") as f:
    f.write(f"{w}x{h}\n")

port.close()
print(f"Captured {w}x{h} ({len(framebuf)} bytes)")
PYEOF

if [ $? -ne 0 ]; then
    echo "Screenshot capture failed"
    exit 1
fi

DIMS=$(cat "$TMPDIMS")
ffmpeg -y -f rawvideo -pixel_format rgb565le -video_size "$DIMS" \
    -i "$TMPRAW" -update 1 -frames:v 1 "$OUTPUT" 2>/dev/null || true


if [ -f "$OUTPUT" ]; then
    echo "Saved: $OUTPUT ($DIMS)"
else
    echo "Error: conversion failed"
    exit 1
fi
