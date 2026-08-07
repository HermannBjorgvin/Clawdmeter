#!/usr/bin/env python3
"""Generate firmware/src/costumes.h — swipe-up/down costumes for the corner Claude.

Original pixel-art props (no third-party assets), drawn to align with the 80x80
corner logo sprite and emitted as RGB565A8. Also writes tools/costumes_preview.png
(props composited on the real logo) as a reference. Run: python tools/gen_costumes.py
"""
import os
import re
from PIL import Image, ImageDraw

W = H = 80
HERE = os.path.dirname(os.path.abspath(__file__))
LOGO = os.path.join(HERE, "..", "firmware", "src", "logo.h")
OUT = os.path.join(HERE, "..", "firmware", "src", "costumes.h")
PREVIEW = os.path.join(HERE, "costumes_preview.png")


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
CRAB = crab.getpixel((40, 30))


def newprop():
    return Image.new("RGBA", (W, H), (0, 0, 0, 0))

# ---- prop drawers (80x80, aligned to the crab: eyes ~x22 & x58 y25-35, head top y17) ----
def shades(im):
    d = ImageDraw.Draw(im); blk = (18,18,20,255)
    d.rectangle([13,24,33,35], fill=blk); d.rectangle([47,24,67,35], fill=blk)
    d.rectangle([33,27,47,31], fill=blk)
    d.rectangle([6,26,13,30], fill=blk); d.rectangle([67,26,74,30], fill=blk)
    d.rectangle([16,26,21,28], fill=(255,255,255,150))

def ears(im):
    d = ImageDraw.Draw(im); dark=(70,35,25,255); pink=(235,150,175,255)
    for x0 in (14, 48):
        d.polygon([(x0,19),(x0+16,19),(x0+8,1)], fill=CRAB, outline=dark)
        d.polygon([(x0+4,16),(x0+12,16),(x0+8,6)], fill=pink)

def sombrero(im):
    d = ImageDraw.Draw(im); straw=(216,178,92,255); band=(184,52,52,255); dark=(120,92,42,255)
    d.ellipse([1,11,79,23], fill=straw, outline=dark)
    d.polygon([(29,16),(51,16),(46,1),(34,1)], fill=straw, outline=dark)
    d.rectangle([30,11,50,15], fill=band)

def beanie(im):
    d = ImageDraw.Draw(im); knit=(64,108,184,255); cuff=(92,138,206,255); pom=(242,242,242,255)
    d.polygon([(14,16),(66,16),(58,3),(22,3)], fill=knit)
    d.rectangle([10,14,70,20], fill=cuff)
    d.ellipse([33,-3,47,11], fill=pom)

def _saber(im, blade, core):
    d = ImageDraw.Draw(im); hilt=(165,165,175,255); dark=(80,80,92,255)
    base, tip = (63, 45), (78, 5)
    d.line([base, tip], fill=blade, width=7)
    d.line([base, tip], fill=core, width=3)
    d.line([base, (57, 61)], fill=hilt, width=9)
    d.line([(56, 53), (64, 50)], fill=dark, width=2)

def saber(im): _saber(im, (96,255,120,255), (232,255,232,255))

def saber_double(im):
    d = ImageDraw.Draw(im)
    blade=(255,70,70,255); core=(255,225,225,255); hilt=(150,150,160,255); dark=(70,70,82,255)
    p0, p1 = (49, 66), (79, 10)
    d.line([p0, p1], fill=blade, width=7)
    d.line([p0, p1], fill=core, width=3)
    d.line([(58, 51), (70, 29)], fill=hilt, width=9)
    d.line([(60, 47), (68, 33)], fill=dark, width=2)

def santa(im):
    d = ImageDraw.Draw(im); red=(202,46,46,255); shad=(150,30,30,255); white=(246,246,246,255)
    d.polygon([(16,15),(52,15),(70,5),(64,2)], fill=red, outline=shad)
    d.rectangle([14,13,56,20], fill=white)
    d.ellipse([62,-1,74,11], fill=white)

def rainbow(im):
    # Stacked filled half-disks (outer→inner) then a transparent inner punch, so
    # each band is the solid ring between two disks — no seams/gaps between bands
    # (concentric thin arcs don't tile pixel-perfectly and leave transparent gaps).
    d = ImageDraw.Draw(im)
    bands = [(228,60,60),(232,140,50),(238,214,70),(86,198,96),(70,140,230),(150,90,220)]
    cx, cy, r = 40, 46, 42
    for c in bands:
        d.pieslice([cx-r, cy-r, cx+r, cy+r], 180, 360, fill=c + (255,)); r -= 3
    d.pieslice([cx-r, cy-r, cx+r, cy+r], 180, 360, fill=(0, 0, 0, 0))  # transparent inner hole

COSTUMES = [("none", None), ("shades", shades), ("cat ears", ears),
            ("sombrero", sombrero), ("pom-pom", beanie), ("lightsaber", saber),
            ("santa", santa), ("double saber", saber_double), ("rainbow", rainbow)]


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
    print("wrote", os.path.relpath(OUT, HERE))

    # reference preview: each costume composited on the real crab
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
    print("wrote", os.path.relpath(PREVIEW, HERE))


if __name__ == "__main__":
    main()
