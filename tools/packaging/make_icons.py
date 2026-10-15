#!/usr/bin/env python3
"""Render the app icons from the official idle Clawd.

Reads the same cells the desktop app draws (frame 0 of `waving` in
firmware/src/splash_animations.h, body rows only; see
desktop/crates/clawdmeter-app/build.rs) and writes, into tools/packaging/icons/:

  clawdmeter-<N>.png   hicolor sizes (Linux .deb / AppImage / Nix)
  clawdmeter.ico       Windows installer + Start Menu shortcut
  clawdmeter.icns      macOS app bundle

Whole cells, no smoothing. 16 px is point-sampled like the tray icon. Only
the standard library, so it runs on every release runner. The outputs are
committed; rerun this when the art changes:

  python3 tools/packaging/make_icons.py
  python3 tools/packaging/make_icons.py --check   # release.yml: pixels match the art
"""

import re
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "firmware/src/splash_animations.h"
OUT = Path(__file__).resolve().parent / "icons"

# Keep in sync with build.rs: the idle body on the 55x37 art stage.
BODY_L, BODY_T, BODY_W, BODY_H = 15, 21, 24, 16
ANIM = "waving"
EYE_INK = 0x141413

PNG_SIZES = [16, 24, 32, 48, 64, 128, 256, 512]
ICO_SIZES = [16, 24, 32, 48, 64, 256]
# (type, pixel size) — PNG-payload ICNS entries.
ICNS_TYPES = [
    (b"icp4", 16), (b"icp5", 32), (b"icp6", 64), (b"ic07", 128),
    (b"ic08", 256), (b"ic09", 512), (b"ic10", 1024),
    (b"ic11", 32), (b"ic12", 64), (b"ic13", 256), (b"ic14", 512),
]


def rgb565_to_888(v):
    r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
    return (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2))


def array_after(src, marker):
    start = src.index(marker)
    body = src[src.index("{", start) + 1 : src.index("}", start)]
    return [int(s, 0) for s in (t.strip() for t in body.split(",")) if s]


def idle_body():
    """Rows of 0xRRGGBB, or None for transparent cells."""
    src = HEADER.read_text()
    row = re.search(r'\{"%s",\s*"[^"]*",\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),' % ANIM, src)
    w, _h, ox, oy = map(int, row.groups())
    palette = array_after(src, f"splash_{ANIM}_palette[")
    frames = array_after(src, f"splash_{ANIM}_frames[")
    rows = []
    for r in range(BODY_H):
        line = []
        for c in range(BODY_W):
            i = frames[(BODY_T - oy + r) * w + (BODY_L - ox + c)]
            if i == 0:
                line.append(None)
            elif palette[i] == 0:
                line.append(EYE_INK)  # the converter's eyes are pure black
            else:
                line.append(rgb565_to_888(palette[i]))
        rows.append(line)
    return rows


def render(body, size):
    """size x size RGBA bytes, body centred at whole cells."""
    if size < BODY_W:
        # Point-sample to `size` columns, as the 16 px tray icon does.
        cols = size
        rows_n = (BODY_H * size + BODY_W - 1) // BODY_W
        cell = lambda x, y: body[y * BODY_W // size][x * BODY_W // size]
        k = 1
    else:
        k = size // BODY_W
        cols, rows_n = BODY_W, BODY_H
        cell = lambda x, y: body[y][x]
    x0 = (size - cols * k) // 2
    y0 = (size - rows_n * k) // 2
    px = bytearray(size * size * 4)
    for y in range(rows_n):
        for x in range(cols):
            rgb = cell(x, y)
            if rgb is None:
                continue
            pixel = bytes(((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 0xFF))
            for dy in range(k):
                i = ((y0 + y * k + dy) * size + x0 + x * k) * 4
                px[i : i + 4 * k] = pixel * k
    return bytes(px)


def png(size, rgba):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    stride = size * 4
    raw = b"".join(b"\x00" + rgba[y * stride : (y + 1) * stride] for y in range(size))
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def ico(images):
    """PNG-payload ICO (Vista+)."""
    head = struct.pack("<HHH", 0, 1, len(images))
    offset = len(head) + 16 * len(images)
    entries, blobs = b"", b""
    for size, data in images:
        dim = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset + len(blobs))
        blobs += data
    return head + entries + blobs


def icns(entries):
    body = b"".join(kind + struct.pack(">I", 8 + len(data)) + data for kind, data in entries)
    return b"icns" + struct.pack(">I", 8 + len(body)) + body


def png_pixels(data):
    """Raw scanlines of one of our PNGs (8-bit RGBA, filter 0), for --check:
    compressed bytes can differ between zlib builds, pixels can't."""
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    i, idat, size = 8, b"", None
    while i < len(data):
        (n,) = struct.unpack(">I", data[i : i + 4])
        kind, payload = data[i + 4 : i + 8], data[i + 8 : i + 8 + n]
        if kind == b"IHDR":
            size = struct.unpack(">II", payload[:8])
        elif kind == b"IDAT":
            idat += payload
        i += 12 + n
    return size, zlib.decompress(idat)


def embedded(path):
    """(size, png bytes) inside our .ico / .icns."""
    data = path.read_bytes()
    out = []
    if path.suffix == ".ico":
        (count,) = struct.unpack("<H", data[4:6])
        for k in range(count):
            dim, _, _, _, _, _, n, off = struct.unpack("<BBBBHHII", data[6 + 16 * k : 22 + 16 * k])
            out.append((dim or 256, data[off : off + n]))
    else:
        i = 8
        while i < len(data):
            (n,) = struct.unpack(">I", data[i + 4 : i + 8])
            out.append((dict(ICNS_TYPES)[data[i : i + 4]], data[i + 8 : i + n]))
            i += n
    return out


def check(body):
    files = [(OUT / f"clawdmeter-{s}.png", s, (OUT / f"clawdmeter-{s}.png").read_bytes()) for s in PNG_SIZES]
    files += [(OUT / "clawdmeter.ico", s, d) for s, d in embedded(OUT / "clawdmeter.ico")]
    files += [(OUT / "clawdmeter.icns", s, d) for s, d in embedded(OUT / "clawdmeter.icns")]
    stale = sorted({str(path.relative_to(ROOT)) for path, size, data in files if png_pixels(data) != png_pixels(png(size, render(body, size)))})
    if len(embedded(OUT / "clawdmeter.ico")) != len(ICO_SIZES) or len(embedded(OUT / "clawdmeter.icns")) != len(ICNS_TYPES):
        stale.append("entry count")
    if stale:
        print("icons differ from the firmware art, rerun tools/packaging/make_icons.py: " + ", ".join(stale))
        return 1
    print(f"icons match the firmware art ({len(files)} images)")
    return 0


def main():
    body = idle_body()
    if sys.argv[1:] == ["--check"]:
        return check(body)
    pngs = {}

    def get(size):
        if size not in pngs:
            pngs[size] = png(size, render(body, size))
        return pngs[size]

    OUT.mkdir(parents=True, exist_ok=True)
    for size in PNG_SIZES:
        (OUT / f"clawdmeter-{size}.png").write_bytes(get(size))
    (OUT / "clawdmeter.ico").write_bytes(ico([(s, get(s)) for s in ICO_SIZES]))
    (OUT / "clawdmeter.icns").write_bytes(icns([(t, get(s)) for t, s in ICNS_TYPES]))
    print(f"wrote {len(PNG_SIZES)} PNGs, clawdmeter.ico, clawdmeter.icns to {OUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
