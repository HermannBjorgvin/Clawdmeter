# AMOLED-2.16 (S3) — `waveshare_amoled_216`

Hardware notes for this board. Shared firmware rules are in `firmware/CLAUDE.md`.

original Waveshare ESP32-S3-Touch-AMOLED-2.16 (CO5300, 480×480 square, CST9220 touch, IMU rotation). Build env: `waveshare_amoled_216`.

## Hardware (critical pins)

- Display: **CO5300** AMOLED via QSPI (CS=12, SCLK=38, SDIO0..3=4..7, RST=2)
- Touch: **CST9220** via I2C (SDA=15, SCL=14, INT=11, addr=0x5A)
- PMU: **AXP2101** on same I2C bus (addr=0x34) — battery, USB VBUS, PWR button IRQ
- IMU: **QMI8658** on same I2C bus (addr=0x6B) — accelerometer for auto-rotation
- Buttons: GPIO 0 (left → Space/voice-mode), GPIO 18 (right → Shift+Tab/mode-toggle), AXP PKEY (middle → PWR: brightness; hold → pair)
- Touch axes: the CST9220 needs `setSwapXY(true)` + `setMirrorXY(true, false)`, applied in `touch.cpp::touch_hal_init()`.
- Rotation: the CO5300 can't exchange rows/columns, so `display_hal_draw_bitmap` remaps pixels on the CPU (PARTIAL render mode, 480×40 strips). A rotation change flashes the AMOLED brightness and forces a full redraw, handled in `display_hal_tick`.
