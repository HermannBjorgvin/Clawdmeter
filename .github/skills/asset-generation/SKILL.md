---
name: asset-generation
description: "Use when adding, regenerating, converting, sizing, or documenting fonts, icons, pixel animations, or other firmware assets."
---

# Asset Generation

Source assets live under `tools/` and generated firmware assets live under `firmware/src/`. Use the existing pipelines:

```powershell
node tools/scrape_claudepix.js
node tools/convert_to_c.js
node tools/png_to_lvgl.js <input.png> <symbol>
```

Never hand-edit `splash_animations.h`. Check LVGL 9 font compatibility, RGB565A8 layout, provenance/licensing, and flash usage after generation. Keep generated changes reproducible from checked-in inputs.
