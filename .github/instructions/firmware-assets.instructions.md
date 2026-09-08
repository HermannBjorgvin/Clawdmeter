---
applyTo: "tools/**,firmware/src/icons.h,firmware/src/*font*.c,firmware/src/splash_animations.h"
description: "Use for generated fonts, icons, pixel animations, converters, and asset provenance."
---

# Firmware Asset Rules

- Treat source JSON, PNG, and converter scripts as authoritative.
- Regenerate generated C headers instead of editing them manually.
- Check LVGL 9 structure compatibility and RGB565A8 memory layout.
- Record provenance and licensing concerns for externally sourced assets.
- Rebuild firmware and inspect flash usage after asset changes.
