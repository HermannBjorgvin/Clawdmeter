# Generate firmware fonts

`firmware/src/font_*.c` contains the LVGL bitmap fonts. Run the converter
from the repository root. The commands below use the bundled font files;
their presence does not grant redistribution rights (see the README's
asset notice).

```bash
npm install -g lv_font_conv@1.5.3

for size in 56 34; do
  lv_font_conv --font assets/TiemposText-400-Regular.otf -r 0x20-0x7E \
    --size "$size" --format lvgl --bpp 4 --no-compress \
    -o "firmware/src/font_tiempos_${size}.c" --lv-include "lvgl.h"
done

for size in 48 28 24 20 16 14 12; do
  lv_font_conv --font assets/StyreneB-Regular.otf -r 0x20-0x7E \
    --size "$size" --format lvgl --bpp 4 --no-compress \
    -o "firmware/src/font_styrene_${size}.c" --lv-include "lvgl.h"
done

for size in 32 18; do
  lv_font_conv --font assets/DejaVuSansMono.ttf \
    -r 0x20-0x7E,0xB7,0x2026,0x2722,0x2733,0x2736,0x273B,0x273D \
    --size "$size" --format lvgl --bpp 4 --no-compress \
    -o "firmware/src/font_mono_${size}.c" --lv-include "lvgl.h"
done
```

Keep `--no-compress`. This converter emits the LVGL 8 structure layout;
match the generated files to the LVGL 9 structures used in this repository:

1. Remove version guards around `font_dsc` and the exported font structure.
2. Remove the `font_dsc.cache` initializer.
3. Set `release_glyph = NULL`, `kerning = 0`, and `static_bitmap = 0` on the font.
4. Set `fallback = NULL` and `user_data = NULL`.

Compare against a checked-in font before replacing it. Build and render the
simulator afterward: a successful compile alone does not prove the glyphs
render. Check the 240×240 layout as well as 480×480. Styrene and Tiempos
contain ASCII only; new symbols need a font with the corresponding glyphs.
