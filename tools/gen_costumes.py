#!/usr/bin/env python3
"""Generate firmware/src/costumes.h — swipe-up/down costumes for the corner Claude.

Original pixel-art props (no third-party assets), drawn on an 80x80 canvas aligned
to the corner logo sprite and emitted as RGB565A8. Also writes two reference PNGs
(both gitignored): tools/costumes_preview.png (every prop on the real logo) and
tools/costumes_guide.png (the logo with a coordinate grid + labeled anatomy points,
to help you place a new prop). Run: python tools/gen_costumes.py

────────────────────────────────────────────────────────────────────────────────
HOW TO ADD A COSTUME
  1. Write a drawer: a function that paints your prop onto an 80x80 RGBA image,
     positioned over the crab. Decorate it with @costume("display name"):

         @costume("top hat")
         def top_hat(im):
             '''A little black top hat above the head.'''
             d = ImageDraw.Draw(im)
             d.rectangle([A.HEAD_L, A.HEAD_TOP - 14, A.HEAD_R, A.HEAD_TOP - 2], fill=(20, 20, 22, 255))
             d.rectangle([A.HEAD_L - 6, A.HEAD_TOP - 3, A.HEAD_R + 6, A.HEAD_TOP], fill=(20, 20, 22, 255))

     Use the A.* anatomy landmarks (below) instead of raw numbers so your prop
     lines up with the crab; run once and check tools/costumes_guide.png to eyeball
     coordinates. Transparent pixels are free — draw only the prop.
  2. Run `python tools/gen_costumes.py`, then check tools/costumes_preview.png.
  3. Rebuild the firmware. No C changes needed — COSTUME_COUNT, the sprite table,
     and the names array are all generated, and ui.cpp is fully data-driven off
     them. New costumes join the swipe cycle automatically.

  Ordering note: costumes cycle in definition order (index 0 = "none"), and the
  selected index is persisted to NVS. Appending new costumes is safe; reordering
  existing ones will change what a saved selection maps to.
────────────────────────────────────────────────────────────────────────────────
"""
import os
import re
from PIL import Image, ImageDraw

W = H = 80
HERE = os.path.dirname(os.path.abspath(__file__))
LOGO = os.path.join(HERE, "..", "firmware", "src", "logo.h")
OUT = os.path.join(HERE, "..", "firmware", "src", "costumes.h")
PREVIEW = os.path.join(HERE, "costumes_preview.png")
GUIDE = os.path.join(HERE, "costumes_guide.png")


def decode_logo():
    txt = open(LOGO).read()
    m = re.search(r"logo_data\[\d+\]\s*=\s*\{(.*?)\}\s*;", txt, re.S)
    vals = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1))]
    n = W * H
    rgb, alpha = vals[:n*2], vals[n*2:n*2+n]
    im = Image.new("RGBA", (W, H)); px = im.load()
    for i in range(n):
        v = rgb[2*i] | (rgb[2*i+1] << 8)
        r = (v >> 11) & 0x1F; g = (v >> 5) & 0x3F; b = v & 0x1F
        px[i % W, i // W] = (r<<3|r>>2, g<<2|g>>4, b<<3|b>>2, alpha[i])
    return im


crab = decode_logo()


class A:
    """Named anatomy landmarks on the 80x80 canvas — where the crab's features sit,
    so props can be positioned meaningfully instead of by guessed pixel numbers.
    (Reference tools/costumes_guide.png for the full grid.)"""
    EYE_L   = 22   # left eye centre x
    EYE_R   = 58   # right eye centre x
    EYE_Y   = 30   # eye centre y (eyes span ~y25..35)
    HEAD_TOP = 17  # top of the head
    HEAD_L  = 14   # head left edge x
    HEAD_R  = 66   # head right edge x
    CX      = 40   # horizontal centre
    COLOR   = crab.getpixel((EYE_L + 18, HEAD_TOP + 13))  # crab body colour (for ears etc.)


CRAB = A.COLOR  # backwards-compatible alias used by the drawers below


def newprop():
    return Image.new("RGBA", (W, H), (0, 0, 0, 0))


# ---- prop drawers -----------------------------------------------------------
# Each paints one prop onto an 80x80 RGBA image aligned to the crab. The
# @costume(name) decorator registers it into the cycle in definition order.
COSTUMES = [("none", None)]   # index 0 is special: no prop


def costume(name):
    def register(fn):
        COSTUMES.append((name, fn))
        return fn
    return register


@costume("shades")
def shades(im):
    """Sunglasses across the eyes, with a bridge, temples, and a glint."""
    d = ImageDraw.Draw(im); blk = (18,18,20,255)
    d.rectangle([13,24,33,35], fill=blk); d.rectangle([47,24,67,35], fill=blk)
    d.rectangle([33,27,47,31], fill=blk)
    d.rectangle([6,26,13,30], fill=blk); d.rectangle([67,26,74,30], fill=blk)
    d.rectangle([16,26,21,28], fill=(255,255,255,150))


@costume("cat ears")
def ears(im):
    """A pair of triangular cat ears (crab-coloured, pink inner) above the head."""
    d = ImageDraw.Draw(im); dark=(70,35,25,255); pink=(235,150,175,255)
    for x0 in (14, 48):
        d.polygon([(x0,19),(x0+16,19),(x0+8,1)], fill=CRAB, outline=dark)
        d.polygon([(x0+4,16),(x0+12,16),(x0+8,6)], fill=pink)


@costume("sombrero")
def sombrero(im):
    """A wide straw sombrero with a red band."""
    d = ImageDraw.Draw(im); straw=(216,178,92,255); band=(184,52,52,255); dark=(120,92,42,255)
    d.ellipse([1,11,79,23], fill=straw, outline=dark)
    d.polygon([(29,16),(51,16),(46,1),(34,1)], fill=straw, outline=dark)
    d.rectangle([30,11,50,15], fill=band)


@costume("pom-pom")
def beanie(im):
    """A knit beanie with a cuff and a white pom-pom."""
    d = ImageDraw.Draw(im); knit=(64,108,184,255); cuff=(92,138,206,255); pom=(242,242,242,255)
    d.polygon([(14,16),(66,16),(58,3),(22,3)], fill=knit)
    d.rectangle([10,14,70,20], fill=cuff)
    d.ellipse([33,-3,47,11], fill=pom)


def _saber(im, blade, core):
    """Shared single-blade lightsaber: an angled blade (glow + core) over a hilt."""
    d = ImageDraw.Draw(im); hilt=(165,165,175,255); dark=(80,80,92,255)
    base, tip = (63, 45), (78, 5)
    d.line([base, tip], fill=blade, width=7)
    d.line([base, tip], fill=core, width=3)
    d.line([base, (57, 61)], fill=hilt, width=9)
    d.line([(56, 53), (64, 50)], fill=dark, width=2)


@costume("lightsaber")
def saber(im):
    """A green single lightsaber."""
    _saber(im, (96,255,120,255), (232,255,232,255))


@costume("santa")
def santa(im):
    """A red santa hat with a white brim and pom-pom."""
    d = ImageDraw.Draw(im); red=(202,46,46,255); shad=(150,30,30,255); white=(246,246,246,255)
    d.polygon([(16,15),(52,15),(70,5),(64,2)], fill=red, outline=shad)
    d.rectangle([14,13,56,20], fill=white)
    d.ellipse([62,-1,74,11], fill=white)


@costume("double saber")
def saber_double(im):
    """Darth Maul's red double-bladed saber, angled across the head."""
    d = ImageDraw.Draw(im)
    blade=(255,70,70,255); core=(255,225,225,255); hilt=(150,150,160,255); dark=(70,70,82,255)
    p0, p1 = (49, 66), (79, 10)
    d.line([p0, p1], fill=blade, width=7)
    d.line([p0, p1], fill=core, width=3)
    d.line([(58, 51), (70, 29)], fill=hilt, width=9)
    d.line([(60, 47), (68, 33)], fill=dark, width=2)


@costume("rainbow")
def rainbow(im):
    """A rainbow arch above the head.

    Stacked filled half-disks (outer→inner) then a transparent inner punch, so
    each band is the solid ring between two disks — no seams/gaps between bands
    (concentric thin arcs don't tile pixel-perfectly and leave transparent gaps).
    """
    d = ImageDraw.Draw(im)
    bands = [(228,60,60),(232,140,50),(238,214,70),(86,198,96),(70,140,230),(150,90,220)]
    cx, cy, r = 40, 46, 42
    for c in bands:
        d.pieslice([cx-r, cy-r, cx+r, cy+r], 180, 360, fill=c + (255,)); r -= 3
    d.pieslice([cx-r, cy-r, cx+r, cy+r], 180, 360, fill=(0, 0, 0, 0))  # transparent inner hole


def to_rgb565a8(im):
    px = im.load(); rgb = bytearray(); al = bytearray()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = px[x, y]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            rgb.append(v & 0xFF); rgb.append((v >> 8) & 0xFF); al.append(a)
    return bytes(rgb) + bytes(al)


def carr(b):
    return "\n".join("    " + ", ".join(f"0x{v:02X}" for v in b[i:i+16]) + ","
                     for i in range(0, len(b), 16))


def write_header(entries):
    with open(OUT, "w") as f:
        f.write("#pragma once\n#include <stdint.h>\n#include <stddef.h>\n\n")
        f.write("// Claude's costume props, RGB565A8. Generated by tools/gen_costumes.py.\n")
        f.write("// Each sprite is cropped to its bounding box; (ox,oy) place it within the\n")
        f.write("// 80x80 corner-logo footprint. Swipe up/down cycles them; index 0 = none.\n")
        f.write(f"#define COSTUME_COUNT {len(COSTUMES)}\n\n")
        f.write("typedef struct {\n")
        f.write("    uint16_t w, h;        // sprite size (bounding box)\n")
        f.write("    int16_t  ox, oy;      // offset within the 80x80 logo footprint\n")
        f.write("    const uint8_t* data;  // RGB565A8, or NULL for \"none\"\n")
        f.write("} costume_t;\n\n")
        for var, w, h, ox, oy, data in entries:
            if data is not None:
                f.write(f"static const uint8_t {var}[{len(data)}] = {{\n{carr(data)}\n}};\n\n")
        f.write("static const costume_t costumes[COSTUME_COUNT] = {\n")
        for var, w, h, ox, oy, data in entries:
            f.write(f"    {{ {w:>2}, {h:>2}, {ox:>2}, {oy:>2}, {var} }},\n")
        f.write("};\n")
        labels = ", ".join(f'"{n}"' for n, _ in COSTUMES)
        f.write(f"static const char* const costume_names[COSTUME_COUNT] = {{ {labels} }};\n")


def write_preview():
    """Every costume composited on the real crab, side by side."""
    SC, pad = 4, 8
    tiles = []
    for name, fn in COSTUMES:
        base = crab.copy()
        if fn:
            p = newprop(); fn(p); base.alpha_composite(p)
        tiles.append(base.resize((W*SC, H*SC), Image.NEAREST))
    cw = W*SC + pad
    sheet = Image.new("RGBA", (cw*len(tiles)+pad, H*SC+pad*2), (30,30,34,255))
    for i, t in enumerate(tiles):
        sheet.alpha_composite(t, (pad + i*cw, pad))
    sheet.save(PREVIEW)


def write_guide():
    """The crab with a 10px coordinate grid + labeled anatomy landmarks, so a new
    prop can be positioned by eye against the A.* points."""
    SC = 8
    base = crab.copy().resize((W*SC, H*SC), Image.NEAREST).convert("RGBA")
    d = ImageDraw.Draw(base)
    for g in range(0, W+1, 10):                       # grid + axis ticks every 10px
        d.line([(g*SC, 0), (g*SC, H*SC)], fill=(255,255,255,40))
        d.line([(0, g*SC), (W*SC, g*SC)], fill=(255,255,255,40))
        d.text((g*SC+2, 1), str(g), fill=(255,255,255,160))
        d.text((1, g*SC+1), str(g), fill=(255,255,255,160))
    def dot(x, y, label):
        d.ellipse([x*SC-4, y*SC-4, x*SC+4, y*SC+4], fill=(255,90,90,255))
        d.text((x*SC+6, y*SC-4), label, fill=(255,180,80,255))
    dot(A.EYE_L, A.EYE_Y, "EYE_L"); dot(A.EYE_R, A.EYE_Y, "EYE_R")
    dot(A.CX, A.HEAD_TOP, "HEAD_TOP")
    dot(A.HEAD_L, A.HEAD_TOP, "HEAD_L"); dot(A.HEAD_R, A.HEAD_TOP, "HEAD_R")
    base.save(GUIDE)


def main():
    # Draw + crop each prop to its bounding box (props are mostly transparent,
    # so this shrinks the stored data ~75% vs a full 80x80 sprite).
    entries = []  # (var|"NULL", w, h, ox, oy, data|None)
    for name, fn in COSTUMES:
        if not fn:
            entries.append(("NULL", 0, 0, 0, 0, None)); continue
        im = newprop(); fn(im)
        bbox = im.getbbox() or (0, 0, 1, 1)
        crop = im.crop(bbox)
        var = "costume_" + name.replace(" ", "_").replace("-", "_") + "_data"
        entries.append((var, crop.width, crop.height, bbox[0], bbox[1], to_rgb565a8(crop)))

    write_header(entries)
    print("wrote", os.path.relpath(OUT, HERE))
    write_preview()
    print("wrote", os.path.relpath(PREVIEW, HERE))
    write_guide()
    print("wrote", os.path.relpath(GUIDE, HERE))


if __name__ == "__main__":
    main()
