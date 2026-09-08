---
applyTo: "firmware/src/ui.cpp,firmware/src/ui.h,firmware/src/theme.h,firmware/src/splash.cpp,firmware/src/splash.h"
description: "Use for LVGL screens, display layout, splash animation, typography, navigation, and backlight behavior."
---

# Firmware UI Rules

- Design against the physical 135x240 viewport and fixed display offsets.
- Preserve screen navigation and valid, stale, offline, and unavailable data states.
- Check text bounds, clipping, overlap, and font size on the real framebuffer.
- Use the serial screenshot workflow for every visual change.
- Avoid adding large fonts or images without checking flash usage.
- Keep animation data in the asset pipeline, not in hand-maintained generated headers.
