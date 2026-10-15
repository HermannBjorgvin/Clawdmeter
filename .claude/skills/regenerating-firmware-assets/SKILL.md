---
name: regenerating-firmware-assets
description: Regenerates the firmware's generated assets - LVGL bitmap fonts (lv_font_conv plus the LVGL 9 patch), RGB565A8 icons from PNGs (tools/png_to_lvgl.js), and the Clawd animation header splash_animations.h (tools/convert_official_clawd.js). Use when adding a font size or glyph, adding or changing an icon, adding or re-converting a Clawd animation, or when text or icons render invisible.
---

# Regenerating firmware assets

Run everything from the repo root. These files are generated: change the
source or the tool, then regenerate. Never hand-edit
`firmware/src/splash_animations.h` or a `font_*.c` beyond the documented
patch.

## Fonts (`firmware/src/font_*.c`)

The exact `lv_font_conv@1.5.3` commands for every font and size are in
[docs/fonts.md](../../../docs/fonts.md). `lv_font_conv` emits the LVGL 8
layout; patch each file for LVGL 9 or **the font compiles but renders
invisible**:

1. Remove the `#if LVGL_VERSION_MAJOR >= 8` guards around `font_dsc` and the font struct.
2. Drop the `.cache` initializer.
3. Set `.release_glyph = NULL`, `.kerning = 0`, `.static_bitmap = 0`.
4. Set `.fallback = NULL`, `.user_data = NULL`.

Diff against a checked-in font to confirm the shape. Styrene and Tiempos are
ASCII only; new symbols need a font that has the glyphs. Then check rendering
in the simulator at 480×480 and 240×240 (`verifying-firmware-ui` skill): a
clean compile proves nothing.

## Icons (`firmware/src/icons.h`)

```bash
(cd tools && npm install --no-save pngjs)        # once; tools/node_modules is git-ignored
node tools/png_to_lvgl.js input.png symbol_name [W_MACRO] [H_MACRO] [--tint=RRGGBB | --no-tint]
```

Output is LVGL RGB565A8, which is **planar**: `w*h` RGB565 pixels, then
`w*h` alpha bytes (`data_size = w*h*3`, `stride = w*2`). Splice it into
`icons.h` and load it with `init_icon_dsc_rgb565a8()` in `ui.cpp`. The
default tint is white because Lucide PNGs are black on transparent and
would render invisible. Only icons that overlap non-uniform backgrounds
(today the battery icons) need RGB565A8; the rest are raw RGB565 baked over
the panel background inside opaque zones.

## Clawd animations (`firmware/src/splash_animations.h`)

```bash
node tools/convert_official_clawd.js                  # regenerate the header
node tools/convert_official_clawd.js --verify DIR     # + per-animation PNGs to eyeball
```

Needs ImageMagick. Sources are the official assets in
`research/clawd-official/` (Laptop and Soccer from their Lottie exports, the
rest from GIFs). What the converter does (55×37 stage crops, ≤16-colour
palettes, loop detection, synthesized eyes, contrast recolors) is in
[tools/README.md](../../../tools/README.md); asset provenance and format
facts are in `research/clawd-official/CLAUDE.md`. Rebuild the firmware
afterwards and check the new or changed animation in the simulator.

Clawd sprites come only from the official art: don't draw or retouch sprite
frames by hand. To find new official animations, use the
`hunting-clawd-animations` skill.
