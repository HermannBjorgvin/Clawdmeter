#!/usr/bin/env python3
"""Capture a connected Clawdmeter board's screen as a PNG.

    screenshot.py [out.png] [--port PORT] [--timeout 15]

Sends the `screenshot` serial command, reads the RGB565 framebuffer the
firmware dumps (`SCREENSHOT_START w h bytes`, raw bytes, `SCREENSHOT_END`)
and writes a PNG sized to the panel. No dependencies beyond pyserial
(PlatformIO's Python has it: ~/.platformio/penv/bin/python). The port
defaults to the only /dev/ttyACM* or /dev/cu.usbmodem* present. Opening the
port doesn't reset the board.

C6 boards answer SCREENSHOT_UNSUPPORTED: they have no PSRAM for the snapshot.
"""

import argparse
import glob
import struct
import sys
import time
import zlib

try:
    import serial
except ImportError:
    sys.exit("pyserial missing: run with ~/.platformio/penv/bin/python, "
             "or pip install pyserial")

BAUD = 115200  # monitor_speed of every env in firmware/platformio.ini


def default_port():
    found = sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/cu.usbmodem*"))
    if len(found) == 1:
        return found[0]
    if not found:
        sys.exit("no board found (no /dev/ttyACM* or /dev/cu.usbmodem*); "
                 "plug one in or pass --port")
    sys.exit(f"several serial ports: {', '.join(found)}; pass --port")


def read_frame(port, timeout):
    """(w, h, raw RGB565 bytes); log lines before the header are skipped."""
    deadline = time.monotonic() + timeout
    while True:
        if time.monotonic() > deadline:
            sys.exit(f"no SCREENSHOT_START within {timeout}s (is the firmware running?)")
        line = port.readline().decode("utf-8", errors="replace").strip()
        if line.startswith("SCREENSHOT_START"):
            _, w, h, size = line.split()
            w, h, size = int(w), int(h), int(size)
            break
        if line == "SCREENSHOT_UNSUPPORTED":
            sys.exit("this board can't take screenshots (no PSRAM, e.g. the C6 boards)")
        if line == "SCREENSHOT_ERR":
            sys.exit("the board failed to take the snapshot (out of PSRAM?)")
    data = bytearray()
    while len(data) < size:
        if time.monotonic() > deadline:
            sys.exit(f"timed out after {len(data)} of {size} bytes")
        data += port.read(min(4096, size - len(data)))
    return w, h, bytes(data)


def to_png(w, h, rgb565):
    """PNG bytes for little-endian RGB565 pixels, 8 bits per channel."""
    # 5/6-bit channel → 8 bits, replicating the high bits into the low ones.
    lut5 = bytes((v << 3) | (v >> 2) for v in range(32))
    lut6 = bytes((v << 2) | (v >> 4) for v in range(64))
    rows = bytearray()
    for y in range(h):
        rows.append(0)  # filter: none
        row = rgb565[y * w * 2:(y + 1) * w * 2]
        for i in range(0, len(row), 2):
            v = row[i] | (row[i + 1] << 8)
            rows += bytes((lut5[v >> 11], lut6[(v >> 5) & 63], lut5[v & 31]))

    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body
                + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(rows), 6))
            + chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out", nargs="?", default="screenshot.png")
    ap.add_argument("--port")
    ap.add_argument("--timeout", type=float, default=15.0,
                    help="seconds for the whole capture (a 480x480 dump takes ~4 s at 115200 baud)")
    args = ap.parse_args()

    port_path = args.port or default_port()
    try:
        port = serial.Serial(port_path, BAUD, timeout=1)
    except serial.SerialException as e:
        sys.exit(f"cannot open {port_path}: {e} (is a serial monitor holding it?)")
    port.reset_input_buffer()
    port.write(b"screenshot\n")
    port.flush()
    w, h, raw = read_frame(port, args.timeout)
    port.close()

    with open(args.out, "wb") as f:
        f.write(to_png(w, h, raw))
    print(f"saved {args.out} ({w}x{h})")


if __name__ == "__main__":
    main()
