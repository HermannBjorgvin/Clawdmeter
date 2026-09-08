---
name: serial-screenshot-qa
description: "Use when validating LVGL screens, layouts, payload rendering, screen navigation, or visual regressions on the connected ST7789 device."
---

# Serial Screenshot QA

Use `tools/screenshot.py` on Windows, or `screenshot.sh` where supported. The firmware supports `screen N`, `feed <json>`, and `screenshot` serial commands.

For a UI change:
1. Build the firmware.
2. Select the target screen with `--screen=N` or `screen N`.
3. Inject representative data with `--feed` where needed.
4. Capture a 135x240 PNG.
5. Inspect clipping, overlap, stale/invalid states, typography, and color contrast.
6. Keep temporary boot-screen changes out of the final diff.

State clearly when hardware is unavailable; host-only compilation is not visual validation.
