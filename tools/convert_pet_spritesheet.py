#!/usr/bin/env python3
"""Convert a ChatGPT pet sprite sheet to firmware/src/pet_animations.h.

The Codex counterpart of convert_official_clawd.js. Sheets follow the format
the ChatGPT pets docs document for custom pets: a transparent PNG or WebP at
1536 x 1872, laid out as an 8 x 9 grid of 192 x 208 frames. Each ROW is one
animation; rows are ragged (6, 8, 8, 4, 5, 8, 6, 6, 6 frames), so occupancy is
detected from alpha rather than assumed.

Unlike the Clawd art -- true pixel art on an integer grid -- these sprites are
anti-aliased with soft shading and drop shadows. There is no native pixel grid
to recover (a run-length scan over the opaque pixels returns gcd 1), so cells
are AREA-downsampled and then quantized, rather than point-sampled at cell
centers the way convert_official_clawd.js does.

All frames are cropped against ONE global content bounding box, not per-frame
boxes. That is what keeps the character anchored across animations, so a
switch from idle to walking does not make it hop.

Requires Pillow, already a project dependency (daemon/icon_assets.py).

Every sheet in the pack shares this layout, so one invocation converts them
all into a single header carrying a PETS[] table the firmware selects from at
runtime.

Usage:
  convert_pet_spritesheet.py SHEET... [--out FILE] [--stage-h N] [--verify DIR]
"""

import argparse
import sys
from collections import Counter
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required: pip install pillow")

COLS, ROWS = 8, 9
CELL_W, CELL_H = 192, 208
PALETTE_SIZE = 16
ALPHA_FLOOR = 8          # below this an source pixel counts as empty
COVERAGE_FLOOR = 0.35    # fraction of a cell that must be opaque to draw it

# Row order in the sheet. Names are what the engine's rate groups and the
# splash picker match against; categories mirror the Clawd header's split.
# The mapping to the pets docs' own states (Running / Needs input / Ready /
# Blocked) is a reading of the artwork, not something the sheet declares.
ROW_ANIMS = [
    ("idle",      "core"),      # 6  - stands, blinks
    ("walking",   "core"),      # 8
    ("running",   "core"),      # 8
    ("waving",    "core"),      # 4
    ("jumping",   "core"),      # 5
    ("dizzy",     "persona"),   # 8  - x_x / >_< faces; reads as "blocked"
    ("thinking",  "persona"),   # 6  - hand to chin
    ("laptop",    "persona"),   # 6  - reads as "running work"
    ("happy",     "persona"),   # 6  - ^_^ faces; reads as "ready"
]

# Per-frame hold. The sheet carries no timing, so these follow the Clawd
# header's convention: a long first frame to settle, then an even gait.
FIRST_HOLD_MS = 330
FRAME_HOLD_MS = 90


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def occupied(cell):
    """Frames present in one row, by alpha."""
    return cell.getchannel("A").point(lambda p: 255 if p > ALPHA_FLOOR else 0).getbbox()


def global_bbox(sheet):
    """One content box covering every frame, so the character stays anchored."""
    x0 = y0 = 10**9
    x1 = y1 = -1
    for r in range(ROWS):
        for c in range(COLS):
            bb = occupied(sheet.crop((c * CELL_W, r * CELL_H,
                                      (c + 1) * CELL_W, (r + 1) * CELL_H)))
            if not bb:
                continue
            x0, y0 = min(x0, bb[0]), min(y0, bb[1])
            x1, y1 = max(x1, bb[2]), max(y1, bb[3])
    return x0, y0, x1, y1


def downsample(frame, stage_w, stage_h):
    """Area-average each cell, returning (rgb, opaque) per cell.

    A cell counts as drawn only if enough of its source area is opaque --
    otherwise the anti-aliased outline smears into a halo of near-transparent
    cells one ring wider than the character.
    """
    small = frame.resize((stage_w, stage_h), Image.BOX)
    px = small.load()
    out = []
    for y in range(stage_h):
        for x in range(stage_w):
            r, g, b, a = px[x, y]
            out.append(((r, g, b), a >= 255 * COVERAGE_FLOOR))
    return out


def rgb565_round(rgb):
    """The color as the panel will actually store it, back in 8-bit channels."""
    return unpack565(rgb565(*rgb))


def unpack565(v):
    """RGB565 back to 8-bit channels, for distance matching in the same space."""
    r = (v >> 11) & 0x1F
    g = (v >> 5) & 0x3F
    b = v & 0x1F
    return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)


def build_palette(all_cells):
    """<=15 colors chosen by median cut over the RGB565-rounded pixels.

    Frequency ranking is the obvious approach and fails on high-contrast art.
    BSOD is mostly white body and blue screen in many near-identical shades,
    so the top 15 by count were seven off-whites within a few units of each
    other, five blues likewise, and the cheek red -- with NO dark entry at
    all. Its dark outline then matched to the red, because red has two low
    channels and beats light grey on distance. Every outline came out scarlet.

    Median cut allocates by how the colors are distributed rather than how
    often they occur, so a small but far-away cluster like an outline keeps a
    slot. Quantizing on the 565-rounded values means near-duplicates the panel
    cannot show anyway collapse before slots are handed out.
    """
    px = [rgb565_round(rgb) for rgb, drawn in all_cells if drawn]
    if not px:
        return []
    strip = Image.new("RGB", (len(px), 1))
    strip.putdata(px)
    q = strip.quantize(colors=PALETTE_SIZE - 1, method=Image.MEDIANCUT)
    pal = q.getpalette()
    used = len(set(q.getdata() if not hasattr(q, 'get_flattened_data')
                   else q.get_flattened_data()))
    return [tuple(pal[i * 3:i * 3 + 3]) for i in range(used)]


def luminance(rgb):
    r, g, b = rgb
    return 0.299 * r + 0.587 * g + 0.114 * b


def lift_palette(palette, min_peak):
    """Brighten a palette whose brightest color is too dark to see on black.

    These sprites are drawn for the pets page, on white. On a true-black AMOLED
    the premise inverts: Null Signal's ENTIRE palette peaks at luminance 48 of
    255 and Stacky's at 101, against a background of 0, so they are all but
    invisible. Every other pet peaks between 139 and 255 and is left alone.

    One multiplier across all channels, so hue and relative shading survive and
    the pet stays the darkest of the set -- just no longer a silhouette against
    its own background.

    This DELIBERATELY departs from the source artwork. Null Signal is drawn to
    be a barely-there shadow and that reads correctly on a white page; it is a
    display adaptation, not a correction, so do not "fix" it back.
    """
    if not min_peak or not palette:
        return palette, 1.0
    peak = max(luminance(c) for c in palette)
    if peak <= 0 or peak >= min_peak:
        return palette, 1.0
    k = min_peak / peak
    return [tuple(min(255, int(round(ch * k))) for ch in c) for c in palette], k


def nearest(rgb, palette):
    r, g, b = rgb
    best, bd = 0, 1 << 30
    for i, (pr, pg, pb) in enumerate(palette):
        d = (r - pr) ** 2 + (g - pg) ** 2 + (b - pb) ** 2
        if d < bd:
            best, bd = i, d
    return best + 1          # +1: index 0 is background


def crop_to_content(frames, stage_w, stage_h):
    """Tighten every frame of one animation to their shared bounding box."""
    x0 = y0 = 10**9
    x1 = y1 = -1
    for cells in frames:
        for i, code in enumerate(cells):
            if not code:
                continue
            x, y = i % stage_w, i // stage_w
            x0, y0 = min(x0, x), min(y0, y)
            x1, y1 = max(x1, x + 1), max(y1, y + 1)
    if x1 < 0:
        return 0, 0, stage_w, stage_h, frames
    w, h = x1 - x0, y1 - y0
    out = []
    for cells in frames:
        out.append([cells[(y0 + y) * stage_w + (x0 + x)]
                    for y in range(h) for x in range(w)])
    return x0, y0, w, h, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sheet", nargs="+")
    ap.add_argument("--out", default="firmware/src/pet_animations.h")
    ap.add_argument("--stage-h", type=int, default=40,
                    help="stage height in cells; width follows the art aspect")
    ap.add_argument("--min-peak", type=int, default=150,
                    help="lift a palette until its brightest entry reaches this "
                         "luminance (0 disables); see lift_palette()")
    ap.add_argument("--verify", help="also write a PNG per animation for eyeballing")
    args = ap.parse_args()

    out = [
        "// " + "=" * 58,
        "// Pet animations - generated by tools/convert_pet_spritesheet.py.",
        "// Source: ChatGPT pet sprite sheets, archived in research/codex-pets/.",
        "// Do not edit by hand - re-run the converter to refresh.",
        "// " + "=" * 58,
        "#pragma once",
        '#include "splash_animations.h"   // splash_anim_def_t, SPLASH_PALETTE_SIZE',
        "",
        "// One selectable pet: its own stage, palette and animation table.",
        "typedef struct {",
        "    const char *name;",
        "    uint8_t stage_w, stage_h;",
        "    uint8_t count;",
        "    const splash_anim_def_t *anims;",
        "} pet_set_t;",
        "",
    ]
    pets = []

    for sheet_path in args.sheet:
        pet = Path(sheet_path).name.split("-spritesheet")[0].replace("-", "_")
        sheet = Image.open(sheet_path).convert("RGBA")
        if sheet.size != (COLS * CELL_W, ROWS * CELL_H):
            sys.exit(f"{sheet_path}: unexpected size {sheet.size}; "
                     f"expected {COLS * CELL_W}x{ROWS * CELL_H}")

        bx0, by0, bx1, by1 = global_bbox(sheet)
        stage_h = args.stage_h
        stage_w = round((bx1 - bx0) / (by1 - by0) * stage_h)

        raw = []
        for r, (name, category) in enumerate(ROW_ANIMS):
            frames = []
            for c in range(COLS):
                cell = sheet.crop((c * CELL_W, r * CELL_H,
                                   (c + 1) * CELL_W, (r + 1) * CELL_H))
                if not occupied(cell):
                    continue
                frames.append(downsample(cell.crop((bx0, by0, bx1, by1)),
                                         stage_w, stage_h))
            raw.append((name, category, frames))

        palette = build_palette([c for _, _, fs in raw for f in fs for c in f])
        palette, lift = lift_palette(palette, args.min_peak)

        defs = []
        for name, category, frames in raw:
            indexed = [[nearest(rgb, palette) if drawn else 0 for rgb, drawn in f]
                       for f in frames]
            ox, oy, w, h, cropped = crop_to_content(indexed, stage_w, stage_h)
            defs.append(dict(name=name, category=category, ox=ox, oy=oy,
                             w=w, h=h, frames=cropped))
            if args.verify:
                d = Path(args.verify) / pet
                d.mkdir(parents=True, exist_ok=True)
                img = Image.new("RGB", (w, h))
                img.putdata([(0, 0, 0) if v == 0 else palette[v - 1]
                             for v in cropped[len(cropped) // 2]])
                img.resize((w * 8, h * 8), Image.NEAREST).save(d / f"{name}.png")

        # Index 0 is the BACKGROUND slot -- the engine renders code 0 fully
        # transparent (splash.cpp: `al = code ? 255 : 0`) and indexes
        # palette[code] directly. Cell codes therefore start at 1, so the real
        # colors must be shifted up by one.
        pal565 = [0x0000] + [rgb565(*c) for c in palette]
        pal565 += [0] * (PALETTE_SIZE - len(pal565))

        out.append(f"static const uint16_t pet_{pet}_palette[SPLASH_PALETTE_SIZE] = {{"
                   + ",".join(f"0x{v:04X}" for v in pal565) + "};")
        for d in defs:
            flat = [v for f in d["frames"] for v in f]
            out.append(f"static const uint8_t pet_{pet}_{d['name']}_frames[{len(flat)}] = {{")
            for f in d["frames"]:
                out.append("    " + ",".join(str(v) for v in f) + ",")
            out.append("};")
            holds = [FIRST_HOLD_MS] + [FRAME_HOLD_MS] * (len(d["frames"]) - 1)
            out.append(f"static const uint16_t pet_{pet}_{d['name']}_holds[{len(holds)}] = {{"
                       + ",".join(str(v) for v in holds) + "};")
        out.append(f"static const splash_anim_def_t pet_{pet}_anims[{len(defs)}] = {{")
        for d in defs:
            n = len(d["frames"])
            out.append(f'    {{ "{d["name"]}", "{d["category"]}", {d["w"]}, {d["h"]}, '
                       f'{d["ox"]}, {d["oy"]}, {n}, 0, {n - 1}, {len(palette) + 1}, '
                       f'pet_{pet}_palette, pet_{pet}_{d["name"]}_frames, '
                       f'pet_{pet}_{d["name"]}_holds }},')
        out.append("};")
        out.append("")

        label = pet.replace("_", " ").title()
        pets.append((pet, label, stage_w, stage_h, len(defs)))
        total = sum(len(d["frames"]) for d in defs)
        note = f", lifted x{lift:.2f}" if lift != 1.0 else ""
        print(f"  {pet:12} {len(defs)} animations, {total} frames, "
              f"stage {stage_w}x{stage_h}, {len(palette)} colors{note}")

    out.append(f"#define PET_COUNT {len(pets)}")
    out.append("static const pet_set_t PETS[PET_COUNT] = {")
    for pet, label, w, h, n in pets:
        out.append(f'    {{ "{label}", {w}, {h}, {n}, pet_{pet}_anims }},')
    out.append("};")

    Path(args.out).write_text("\n".join(out) + "\n")
    kb = Path(args.out).stat().st_size // 1024
    print(f"{args.out}: {len(pets)} pets, {kb} KB")


if __name__ == "__main__":
    main()
