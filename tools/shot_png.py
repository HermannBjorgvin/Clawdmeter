#!/usr/bin/env python3
# Capture the LVGL framebuffer via the firmware's `screenshot` serial command
# and write a PNG using only the Python standard library (no ffmpeg / PIL).
# Usage: shot_png.py <port> <out.png>
import sys, struct, zlib, serial

port_path, out_path = sys.argv[1], sys.argv[2]
# Open without asserting DTR/RTS so we don't reset the board (RTS is wired to
# EN on this board) — keeps live BLE data on screen for the capture.
port = serial.Serial()
port.port = port_path
port.baudrate = 115200
port.timeout = 12
port.dtr = False
port.rts = False
port.open()
port.reset_input_buffer()
port.write(b"screenshot\n")
port.flush()

w = h = raw_size = 0
while True:
    line = port.readline().decode("utf-8", "replace").strip()
    if line.startswith("SCREENSHOT_START"):
        _, w, h, raw_size = line.split()
        w, h, raw_size = int(w), int(h), int(raw_size)
        break
    if line == "SCREENSHOT_ERR":
        print("device reported SCREENSHOT_ERR", file=sys.stderr); sys.exit(1)

data = b""
while len(data) < raw_size:
    chunk = port.read(min(4096, raw_size - len(data)))
    if not chunk:
        print(f"timeout: {len(data)}/{raw_size} bytes", file=sys.stderr); sys.exit(1)
    data += chunk
port.close()

# RGB565 little-endian -> RGB888 scanlines (each row prefixed with filter byte 0)
rows = bytearray()
for y in range(h):
    rows.append(0)
    base = y * w * 2
    for x in range(w):
        px = data[base + x*2] | (data[base + x*2 + 1] << 8)
        r = (px >> 11) & 0x1F; g = (px >> 5) & 0x3F; b = px & 0x1F
        rows += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))

def chunk(tag, body):
    c = tag + body
    return struct.pack(">I", len(body)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)

png = b"\x89PNG\r\n\x1a\n"
png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(bytes(rows), 9))
png += chunk(b"IEND", b"")
with open(out_path, "wb") as f:
    f.write(png)
print(f"wrote {out_path} ({w}x{h})")
