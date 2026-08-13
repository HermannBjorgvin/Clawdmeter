#!/bin/bash
# Take a screenshot from the device via the firmware's LVGL snapshot command.
# Usage: ./screenshot.sh [output.png] [port]
# Default port: /dev/cu.usbmodem101 on macOS, /dev/ttyACM0 on Linux.
# On Windows (Git Bash) pass the COM port explicitly: ./screenshot.sh out.png COM8

OUTPUT="${1:-screenshot.png}"
if [ -z "$2" ]; then
    case "$(uname -s)" in
        Darwin) PORT="/dev/cu.usbmodem101" ;;
        *)      PORT="/dev/ttyACM0" ;;
    esac
else
    PORT="$2"
fi

# Use pio's bundled python if pyserial isn't on the system python. On Windows
# `python3` is usually the Microsoft Store stub, which fails with an opaque
# pymanager internal error — hence the Scripts/ path in the fallback list.
PY="python3"
if ! python3 -c "import serial" 2>/dev/null; then
    for CAND in "$HOME/.platformio/penv/bin/python" "$HOME/.platformio/penv/Scripts/python.exe"; do
        if [ -x "$CAND" ]; then PY="$CAND"; break; fi
    done
fi

echo "Taking screenshot from $PORT..."

"$PY" - "$PORT" "$OUTPUT" << 'PYEOF'
import serial, sys, zlib, struct

port_path, out_path = sys.argv[1], sys.argv[2]

# Open with DTR/RTS deasserted. On boards whose serial goes through a USB-UART
# bridge (CH9102/CP2102/CH340) those lines are wired to EN/IO0, so opening the
# port the default way resets the chip and the screenshot command is answered
# by a rebooting device — i.e. never. Native-USB boards don't care either way.
port = serial.Serial()
port.port = port_path
port.baudrate = 115200
port.timeout = 10
port.dtr = False
port.rts = False
port.open()

port.reset_input_buffer()
port.write(b"screenshot\n")
port.flush()

w = h = raw_size = 0
while True:
    line = port.readline().decode("utf-8", errors="replace").strip()
    if line.startswith("SCREENSHOT_START"):
        parts = line.split()
        w, h, raw_size = int(parts[1]), int(parts[2]), int(parts[3])
        break
    if line == "SCREENSHOT_ERR":
        print("Device reported screenshot error", file=sys.stderr)
        sys.exit(1)
    if line == "SCREENSHOT_UNSUPPORTED":
        print("This build has LV_USE_SNAPSHOT=0 (no framebuffer to spare)", file=sys.stderr)
        sys.exit(1)
    if not line:
        print("No response — is the device running Clawdmeter firmware?", file=sys.stderr)
        sys.exit(1)

data = b""
while len(data) < raw_size:
    chunk = port.read(min(4096, raw_size - len(data)))
    if not chunk:
        print(f"Timeout: got {len(data)} of {raw_size} bytes", file=sys.stderr)
        sys.exit(1)
    data += chunk

for _ in range(10):
    if port.readline().decode("utf-8", errors="replace").strip() == "SCREENSHOT_END":
        break
port.close()

# RGB565 little-endian -> PNG, with stdlib only. This used to shell out to
# ffmpeg, which isn't installed on plenty of machines (including Windows boxes
# where the rest of the toolchain works fine) and was the only external
# dependency this script had.
rows = bytearray()
for y in range(h):
    rows.append(0)                      # PNG filter type: none
    base = y * w * 2
    for x in range(w):
        v = data[base + 2 * x] | (data[base + 2 * x + 1] << 8)
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        rows += bytes((r * 255 // 31, g * 255 // 63, b * 255 // 31))

def chunk_of(tag, payload):
    return (struct.pack(">I", len(payload)) + tag + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

png = b"\x89PNG\r\n\x1a\n"
png += chunk_of(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
png += chunk_of(b"IDAT", zlib.compress(bytes(rows), 9))
png += chunk_of(b"IEND", b"")
with open(out_path, "wb") as f:
    f.write(png)

print(f"Saved: {out_path} ({w}x{h}, {len(data)} bytes raw)")
PYEOF

if [ $? -ne 0 ]; then
    echo "Screenshot capture failed"
    exit 1
fi
