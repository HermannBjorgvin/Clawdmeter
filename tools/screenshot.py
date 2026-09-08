#!/usr/bin/env python3
"""Capture the LVGL framebuffer from the device over serial and save a PNG.

Cross-platform, pure standard library (only pyserial is required) — no ffmpeg
or Pillow. Handy on Windows where screenshot.sh (bash + ffmpeg) doesn't run.

Usage:
    python tools/screenshot.py [output.png] [port]

Defaults: output "screenshot.png", port COM13 on Windows / /dev/ttyACM0 else.
"""
import struct
import sys
import time
import zlib

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial not found — `pip install pyserial` "
             "or run with %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe")


def default_port() -> str:
    return "COM13" if sys.platform.startswith("win") else "/dev/ttyACM0"


def capture(port_path: str, screen: str | None = None,
            feeds: list[str] | None = None,
            cmds: list[str] | None = None) -> tuple[int, int, bytes]:
    port = serial.Serial(port_path, 115200, timeout=2)
    try:
        # Opening the port toggles DTR/RTS on many adapters, resetting the
        # ESP32 — give it a moment to boot before sending the command.
        time.sleep(2.5)
        port.reset_input_buffer()

        for payload in feeds or []:
            port.write(b"feed " + payload.encode() + b"\n")
            port.flush()
            time.sleep(0.3)

        for raw in cmds or []:
            port.write(raw.encode() + b"\n")
            port.flush()
            time.sleep(0.3)

        if screen is not None:
            port.write(f"screen {screen}\n".encode())
            port.flush()
            time.sleep(0.8)
            port.reset_input_buffer()

        def send() -> None:
            port.write(b"screenshot\n")
            port.flush()

        def read_exact(n: int) -> bytes:
            out = bytearray()
            while len(out) < n:
                chunk = port.read(n - len(out))
                if not chunk:
                    sys.exit(f"timeout: got {len(out)} of {n} bytes")
                out += chunk
            return bytes(out)

        send()
        w = h = 0
        deadline = time.time() + 20
        resent = False
        while time.time() < deadline:
            line = port.readline().decode("utf-8", "replace").strip()
            if line.startswith("SCREENSHOT_START"):
                _, sw, sh, _ss = line.split()
                w, h = int(sw), int(sh)
                break
            if line.startswith("SCREENSHOT_ERR"):
                sys.exit(f"device: {line}")
            if not resent and ("ready" in line or "Dashboard" in line):
                resent = True  # device rebooted on open — ask again
                send()
        else:
            sys.exit("no SCREENSHOT_START from device (wrong port, or it's busy)")

        # The device streams the frame as tiles: an "A x1 y1 x2 y2" header line,
        # then (tw*th*2) RGB565LE bytes, then a newline. Ends with SCREENSHOT_END.
        port.timeout = 10
        frame = bytearray(w * h * 2)
        while True:
            line = port.readline().decode("utf-8", "replace").strip()
            if not line:
                continue
            if line == "SCREENSHOT_END":
                break
            if not line.startswith("A "):
                continue
            _, x1, y1, x2, y2 = line.split()
            x1, y1, x2, y2 = int(x1), int(y1), int(x2), int(y2)
            tw, th = x2 - x1 + 1, y2 - y1 + 1
            tile = read_exact(tw * th * 2)
            port.read(1)  # trailing newline
            for row in range(th):
                dst = ((y1 + row) * w + x1) * 2
                src = row * tw * 2
                frame[dst:dst + tw * 2] = tile[src:src + tw * 2]
        return w, h, bytes(frame)
    finally:
        port.close()


def rgb565le_to_rgb888(w: int, h: int, raw: bytes) -> bytes:
    out = bytearray(w * h * 3)
    for i in range(w * h):
        v = raw[i * 2] | (raw[i * 2 + 1] << 8)
        r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
        o = i * 3
        out[o] = (r * 255 + 15) // 31
        out[o + 1] = (g * 255 + 31) // 63
        out[o + 2] = (b * 255 + 15) // 31
    return bytes(out)


def write_png(path: str, w: int, h: int, rgb: bytes) -> None:
    def chunk(tag: bytes, body: bytes) -> bytes:
        return (struct.pack(">I", len(body)) + tag + body
                + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))

    rows = bytearray()
    stride = w * 3
    for y in range(h):
        rows.append(0)  # filter: none
        rows += rgb[y * stride:(y + 1) * stride]

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(rows), 9))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def main() -> None:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = [a for a in sys.argv[1:] if a.startswith("--")]
    screen = next((a.split("=", 1)[1] for a in opts if a.startswith("--screen=")), None)
    feeds = [a.split("=", 1)[1] for a in opts if a.startswith("--feed=")]
    cmds = [a.split("=", 1)[1] for a in opts if a.startswith("--cmd=")]
    out = args[0] if args else "screenshot.png"
    port = args[1] if len(args) > 1 else default_port()
    print(f"Capturing from {port} ...")
    w, h, raw = capture(port, screen, feeds, cmds)
    write_png(out, w, h, rgb565le_to_rgb888(w, h, raw))
    print(f"Saved {out} ({w}x{h})")


if __name__ == "__main__":
    main()
