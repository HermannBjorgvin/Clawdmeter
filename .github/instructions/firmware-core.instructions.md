---
applyTo: "firmware/platformio.ini,firmware/src/**"
description: "Use for ESP32 firmware implementation, build, memory, and module-boundary changes in Clawdmeter."
---

# Firmware Core Rules

- Read `AGENTS.md` and `CLAUDE.md` before changing firmware.
- Preserve the BLE/daemon contract, public module signatures, and the 135x240 ST7789 target unless the request explicitly changes them.
- Respect no-PSRAM SRAM limits and the tight flash budget.
- Keep generated headers reproducible; do not hand-edit generated assets.
- Make the smallest bounded change and run `pio run -d firmware` before widening scope.
- Report hardware-dependent checks separately from host-only checks.
