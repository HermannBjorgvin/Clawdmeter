#!/usr/bin/env python3
"""Make the digits in a pre-compiled LVGL font tabular (fixed-advance).

Styrene and Tiempos are proportional faces: in font_styrene_48.c the glyph
'1' advances 342 units against '0' at 531, a 55% difference. Every changing
number on the device therefore jitters -- "42%" and "11%" are different
widths, so the percent sign and everything after it shifts as usage ticks.

The obvious fix, regenerating with OpenType `tnum`, does not work here:
lv_font_conv converts glyphs BY CODEPOINT and does not apply OpenType
features, and tabular figures are a feature rather than separate codepoints.
So equalize the advance directly in the generated table instead -- widen every
digit to the widest one and shift its bitmap right by half the difference, so
the glyph stays optically centred in its new cell. Shapes are untouched; only
the spacing around them changes.

Idempotent: running twice is a no-op, since the digits are already equal.

Usage:  tabular_digits.py firmware/src/font_styrene_48.c [...]
"""

import re
import sys
from pathlib import Path

GLYPH_RE = re.compile(
    r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), "
    r"\.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}")

# format0_tiny cmaps in these files start at U+0020 with glyph id 1, so the
# glyph index of a character is 1 + (codepoint - 0x20).
FIRST_CP = 0x20
DIGIT_IDS = [1 + (ord(d) - FIRST_CP) for d in "0123456789"]


def retabulate(path: Path) -> str:
    src = path.read_text()
    start = src.index("glyph_dsc[] = {")
    end = src.index("};", start)
    body = src[start:end]

    glyphs = list(GLYPH_RE.finditer(body))
    widths = {i: int(glyphs[i].group(2)) for i in DIGIT_IDS if i < len(glyphs)}
    if not widths:
        return f"{path.name}: no digit glyphs found"
    target = max(widths.values())
    if len(set(widths.values())) == 1:
        return f"{path.name}: already tabular ({target})"

    out = body
    for i in DIGIT_IDS:
        m = glyphs[i]
        bi, adv, bw, bh, ox, oy = (int(g) for g in m.groups())
        if adv == target:
            continue
        # adv_w is 1/16 px; ofs_x is whole px. Half the added width, rounded.
        ox += int(round((target - adv) / 2 / 16))
        new = (f"{{.bitmap_index = {bi}, .adv_w = {target}, .box_w = {bw}, "
               f".box_h = {bh}, .ofs_x = {ox}, .ofs_y = {oy}}}")
        out = out.replace(m.group(0), new, 1)

    path.write_text(src[:start] + out + src[end:])
    return (f"{path.name}: digits {min(widths.values())}..{max(widths.values())} "
            f"-> {target}")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__.strip().splitlines()[-1])
    for arg in sys.argv[1:]:
        print(retabulate(Path(arg)))


if __name__ == "__main__":
    main()
