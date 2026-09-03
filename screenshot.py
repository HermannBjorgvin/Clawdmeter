#!/usr/bin/env python3
"""Capture the device's LVGL framebuffer over serial and save it as a PNG.

Cross-platform replacement for screenshot.sh, which needed bash plus ffmpeg and
so could not run from PowerShell.

    py screenshot.py                        # auto-detect port, write screenshot.png
    py screenshot.py sessions.png           # auto-detect port, chosen filename
    py screenshot.py sessions.png COM4      # explicit port (same args as the .sh)
    py screenshot.py --list                 # show candidate serial ports

Only needs pyserial. PNG encoding uses stdlib zlib rather than Pillow or ffmpeg,
so there is nothing extra to install on a fresh machine. If pyserial is missing
this re-executes itself with PlatformIO's bundled Python, which has it -- the same
trick screenshot.sh used.

Protocol (see send_screenshot() in firmware/src/main.cpp):
    host  -> "screenshot\\n"
    device -> "SCREENSHOT_START <w> <h> <bytes>"
           -> <bytes> of RGB565, little-endian, top-left origin
           -> blank line, then "SCREENSHOT_END"
    or     -> "SCREENSHOT_ERR"          (framebuffer allocation/snapshot failed)
    or     -> "SCREENSHOT_UNSUPPORTED"  (no PSRAM: the C6 boards)
"""
from __future__ import annotations

import argparse
import array
import os
import struct
import sys
import zlib
from pathlib import Path

BAUD = 115200
READ_TIMEOUT = 10.0
ESPRESSIF_VID = 0x303A     # ESP32-S3 / C6 native USB-JTAG


# --- pyserial bootstrap ---------------------------------------------------

def _reexec_with_platformio_python() -> None:
    """Re-run under PlatformIO's Python, which ships pyserial.

    Mirrors screenshot.sh's fallback. Guarded by an env var so a broken
    interpreter cannot cause an exec loop.
    """
    if os.environ.get("_CLAWD_SHOT_REEXEC"):
        return
    candidates = []
    for base in (os.environ.get("PLATFORMIO_CORE_DIR"), Path.home() / ".platformio"):
        if not base:
            continue
        base = Path(base)
        candidates += [base / "penv" / "Scripts" / "python.exe",
                       base / "penv" / "bin" / "python"]
    for py in candidates:
        if py.is_file():
            env = dict(os.environ, _CLAWD_SHOT_REEXEC="1")
            os.execve(str(py), [str(py), os.path.abspath(__file__)] + sys.argv[1:], env)


try:
    import serial
    from serial.tools import list_ports
except ImportError:
    _reexec_with_platformio_python()
    sys.exit("pyserial not found. Install it with:  py -m pip install pyserial")


# --- PNG encoding (stdlib only) -------------------------------------------

def _png_chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def write_png(path: Path, width: int, height: int, rgb: bytes) -> None:
    """Write 8-bit truecolour PNG from packed RGB888 rows."""
    stride = width * 3
    raw = bytearray()
    for y in range(height):
        raw.append(0)                     # filter type 0 (None) per scanline
        raw += rgb[y * stride:(y + 1) * stride]
    png = b"\x89PNG\r\n\x1a\n"
    png += _png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += _png_chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += _png_chunk(b"IEND", b"")
    path.write_bytes(png)


def rgb565_to_rgb888(data: bytes) -> bytes:
    """Expand little-endian RGB565 to RGB888.

    Channels are scaled by bit replication ((v << 3) | (v >> 2)) rather than a
    plain shift, so full-scale 5-bit 0x1F maps to 255 and not 248 -- otherwise
    white on the panel renders as a slightly grey #F8F8F8 in the capture.
    """
    px = array.array("H")
    px.frombytes(data)
    if sys.byteorder != "little":
        px.byteswap()
    out = bytearray(len(px) * 3)
    for i, v in enumerate(px):
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        j = i * 3
        out[j] = (r << 3) | (r >> 2)
        out[j + 1] = (g << 2) | (g >> 4)
        out[j + 2] = (b << 3) | (b >> 2)
    return bytes(out)


# --- Serial ---------------------------------------------------------------

def candidate_ports() -> list[str]:
    """Serial ports that look like the board, best guess first."""
    ports = list(list_ports.comports())
    espressif = [p.device for p in ports if p.vid == ESPRESSIF_VID]
    if espressif:
        return espressif
    # Fall back to the platform conventions the .sh hardcoded.
    if sys.platform == "darwin":
        return [p.device for p in ports if "usbmodem" in p.device]
    return [p.device for p in ports if "ttyACM" in p.device or "ttyUSB" in p.device]


def open_port(path: str) -> serial.Serial:
    """Open without asserting DTR/RTS.

    Critical: on the ESP32-S3's native USB-JTAG, DTR/RTS transitions can reset the
    chip or drop it into the bootloader. A reset would reboot the device to the
    splash screen -- destroying the very screen we are trying to capture.
    """
    port = serial.Serial()
    port.port = path
    port.baudrate = BAUD
    port.timeout = READ_TIMEOUT
    port.dtr = False
    port.rts = False
    port.open()
    return port


def capture(path: str) -> tuple[int, int, bytes]:
    port = open_port(path)
    try:
        port.reset_input_buffer()
        port.write(b"screenshot\n")
        port.flush()

        width = height = size = 0
        # The device may still be emitting unrelated log lines, so scan rather
        # than assuming the reply is the very next line.
        for _ in range(200):
            line = port.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue
            if line.startswith("SCREENSHOT_START"):
                parts = line.split()
                if len(parts) < 4:
                    raise RuntimeError(f"malformed header: {line!r}")
                width, height, size = int(parts[1]), int(parts[2]), int(parts[3])
                break
            if line == "SCREENSHOT_UNSUPPORTED":
                raise RuntimeError(
                    "Device reports screenshot unsupported. The C6 boards have no "
                    "PSRAM to hold a full framebuffer (LV_USE_SNAPSHOT=0), so UI "
                    "changes there have to be checked on the panel by eye."
                )
            if line == "SCREENSHOT_ERR":
                raise RuntimeError("Device reported a snapshot error (PSRAM alloc failed?)")
        else:
            raise RuntimeError(
                "No SCREENSHOT_START seen. Is this the right port, and is the "
                "firmware running (not held in the bootloader)?"
            )

        expected = width * height * 2
        if size != expected:
            raise RuntimeError(
                f"header says {size} bytes but {width}x{height} RGB565 needs {expected}"
            )

        chunks: list[bytes] = []
        got = 0
        while got < size:
            chunk = port.read(min(8192, size - got))
            if not chunk:
                raise RuntimeError(f"timed out after {got} of {size} bytes")
            chunks.append(chunk)
            got += len(chunk)

        for _ in range(10):
            if port.readline().decode("utf-8", errors="replace").strip() == "SCREENSHOT_END":
                break

        return width, height, b"".join(chunks)
    finally:
        port.close()


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Capture the Clawdmeter display over serial as a PNG.")
    ap.add_argument("output", nargs="?", default="screenshot.png",
                    help="output PNG path (default: screenshot.png)")
    ap.add_argument("port", nargs="?", default=None,
                    help="serial port, e.g. COM4 or /dev/ttyACM0 (default: auto-detect)")
    ap.add_argument("--list", action="store_true", help="list candidate ports and exit")
    args = ap.parse_args()

    if args.list:
        for p in list_ports.comports():
            vid = f"{p.vid:04X}" if p.vid else "----"
            mark = "  <-- board" if p.vid == ESPRESSIF_VID else ""
            print(f"{p.device:10} VID:{vid}  {p.description}{mark}")
        return 0

    port = args.port
    if port is None:
        found = candidate_ports()
        if not found:
            print("No board found. Use --list to see ports, then pass one explicitly.",
                  file=sys.stderr)
            return 1
        port = found[0]
        if len(found) > 1:
            print(f"Multiple candidates {found}; using {port}")

    print(f"Capturing from {port}...")
    try:
        width, height, data = capture(port)
    except (RuntimeError, OSError, serial.SerialException) as e:
        print(f"Capture failed: {e}", file=sys.stderr)
        return 1

    out = Path(args.output)
    write_png(out, width, height, rgb565_to_rgb888(data))
    print(f"Saved {out} ({width}x{height}, {len(data)} bytes raw)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
