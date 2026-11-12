#!/usr/bin/env python3
"""Render the background of the macOS disk image's window.

The window shows Clawdmeter.app on the left and the Applications link on
the right (macos/dmg-DS_Store places them; make_dmg_layout.sh writes it from
the numbers here). This draws what sits between and behind them, in the
device's default palette: the panel grey, a plate in the text colour under
the icons, and on it a row of meter cells that fills towards Applications
and ends in an arrowhead. Finder draws the icons' names dark whatever the
picture, so they need the light plate. Whole cells, no smoothing, no text
and no Clawd (the app's icon is the one Clawd in the window).

Writes, into tools/packaging/macos/:

  dmg-background.png      640x360, the window's content size in points
  dmg-background@2x.png   the same at 2x; dmg.sh joins the two into a TIFF

Only the standard library. The outputs are committed; rerun this when the
picture changes, and make_dmg_layout.sh too when a size or position does:

  python3 tools/packaging/make_dmg_background.py
  python3 tools/packaging/make_dmg_background.py --check    # release.yml: pictures and layout match
  python3 tools/packaging/make_dmg_background.py --layout   # the numbers, for make_dmg_layout.sh
"""

import re
import struct
import sys
import zlib
from pathlib import Path

OUT = Path(__file__).resolve().parent / "macos"
LAYOUT = OUT / "dmg-DS_Store"
TITLE_BAR = 28  # Finder's window height includes it

# The window's content, in points. macos/dmg-DS_Store holds the same numbers.
WIDTH, HEIGHT = 640, 360
ICON_Y = 168  # centre of both icons
APP_X, APPLICATIONS_X = 176, 464  # their centres

# The device's default theme (firmware theme_palette.h).
PANEL = (0x1F, 0x1F, 0x1E)
TEXT = (0xFA, 0xF9, 0xF5)
ACCENT = (0xD9, 0x77, 0x57)

# The plate: the icons, their names and the arrow sit on it.
PLATE = (56, 48, 584, 320)  # left, top, right, bottom
CORNER = 4  # its corners step in by this, twice
TRACK = (0xE4, 0xE1, 0xD8)  # an empty meter cell on the plate

CELL, PITCH = 8, 12  # a meter cell and the step between cells
SHAFT_CELLS = 7


def mix(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def plate():
    """(x, y, w, h) rectangles that make the plate, corners stepped."""
    left, top, right, bottom = PLATE
    c = CORNER
    return [
        (left + 2 * c, top, right - left - 4 * c, c),
        (left + c, top + c, right - left - 2 * c, c),
        (left, top + 2 * c, right - left, bottom - top - 4 * c),
        (left + c, bottom - 2 * c, right - left - 2 * c, c),
        (left + 2 * c, bottom - c, right - left - 4 * c, c),
    ]


def cells():
    """(x, y, colour) of each cell's top-left corner, in points."""
    head = 2  # columns the arrowhead adds past the shaft's tip
    span = (SHAFT_CELLS + head - 1) * PITCH + CELL
    left = (APP_X + APPLICATIONS_X - span) // 2
    top = ICON_Y - CELL // 2
    out = []
    # The shaft fills like a usage bar: empty track at the app's side, full
    # at the tip.
    for i in range(SHAFT_CELLS + head):
        t = min(1.0, i / (SHAFT_CELLS - 1))
        out.append((left + i * PITCH, top, mix(TRACK, ACCENT, t)))
    # The head: two cells stepping back and out from the tip, each side.
    tip = left + (SHAFT_CELLS + head - 1) * PITCH
    for step in (1, 2):
        for side in (-1, 1):
            out.append((tip - step * PITCH, top + side * step * PITCH, ACCENT))
    return out


def render(scale):
    w, h = WIDTH * scale, HEIGHT * scale
    rows = [bytearray(bytes(PANEL) * w) for _ in range(h)]
    rects = [(x, y, pw, ph, TEXT) for x, y, pw, ph in plate()]
    rects += [(x, y, CELL, CELL, colour) for x, y, colour in cells()]
    for x, y, rw, rh, colour in rects:
        run = bytes(colour) * (rw * scale)
        for row in rows[y * scale : (y + rh) * scale]:
            row[x * scale * 3 : (x + rw) * scale * 3] = run
    return w, h, rows


def png(w, h, rows):
    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    raw = b"".join(b"\x00" + bytes(row) for row in rows)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def png_pixels(data):
    """Size and raw scanlines of one of our PNGs, for --check: compressed
    bytes can differ between zlib builds, pixels can't."""
    pos, size, idat = 8, None, b""
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos : pos + 4])
        kind, body = data[pos + 4 : pos + 8], data[pos + 8 : pos + 8 + length]
        if kind == b"IHDR":
            size = struct.unpack(">II", body[:8])
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    return size, zlib.decompress(idat)


def layout_problems():
    """What macos/dmg-DS_Store (Finder's layout) says differently from the
    numbers here: the two icons' centres and the window's size."""
    if not LAYOUT.exists():
        return [f"{LAYOUT.name} is missing"]
    data = LAYOUT.read_bytes()
    problems = []
    for name, want in (("Clawdmeter.app", (APP_X, ICON_Y)), ("Applications", (APPLICATIONS_X, ICON_Y))):
        # An icon's record: its name in UTF-16, then Iloc, blob, 16, x, y.
        at = data.find(name.encode("utf-16-be") + b"Ilocblob\x00\x00\x00\x10")
        have = struct.unpack(">II", data[at + len(name) * 2 + 12 :][:8]) if at >= 0 else None
        if have != want:
            problems.append(f"{name} is at {have}, not {want}")
    bounds = re.search(rb"\{\{\d+, \d+\}, \{(\d+), (\d+)\}\}", data)
    have = tuple(map(int, bounds.groups())) if bounds else None
    if have != (WIDTH, HEIGHT + TITLE_BAR):
        problems.append(f"the window is {have}, not {(WIDTH, HEIGHT + TITLE_BAR)}")
    return problems


def outputs():
    return {
        OUT / "dmg-background.png": png(*render(1)),
        OUT / "dmg-background@2x.png": png(*render(2)),
    }


def main():
    if sys.argv[1:] == ["--layout"]:
        print(WIDTH, HEIGHT, APP_X, APPLICATIONS_X, ICON_Y)
        return
    files = outputs()
    if sys.argv[1:] == ["--check"]:
        stale = [
            p.name for p, data in files.items() if not p.exists() or png_pixels(p.read_bytes()) != png_pixels(data)
        ]
        if stale:
            sys.exit(f"stale: {', '.join(stale)}; run tools/packaging/make_dmg_background.py")
        problems = layout_problems()
        if problems:
            sys.exit(f"{LAYOUT.name}: {'; '.join(problems)}; run tools/packaging/make_dmg_layout.sh on a Mac")
        print("dmg background and layout match")
        return
    OUT.mkdir(exist_ok=True)
    for path, data in files.items():
        path.write_bytes(data)
        print(f"wrote {path.relative_to(OUT.parent.parent.parent)}")


if __name__ == "__main__":
    main()
