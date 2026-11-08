# AMOLED-1.32 — `waveshare_amoled_132`

Hardware notes for this board. Shared firmware rules are in `firmware/CLAUDE.md`.

Waveshare ESP32-S3-Touch-AMOLED-1.32 (CO5300 466×466 **round** AMOLED over QSPI, CST820 touch @ 0x15, ESP32-S3-PICO-1-N8R8: 8 MB flash + 8 MB octal PSRAM, ES8311 codec unused). Build env: `waveshare_amoled_132`. The first round panel: `round_panel = true`, so the meters keep inside the circle (`layout.cpp`) and the full-size Clawd enters and leaves along the circle (see `docs/clawd-director.md`).

- **No PMU, no IMU.** The battery is a 2:1 divider into GPIO 4 (ADC1_CH3), and `BAT_EN` (GPIO 18) is the power-hold latch, driven HIGH in `board_init()` (same shape as the LCD-1.54). No charge-status pin.
- The CO5300's 466-wide viewport starts at GRAM column 6: `CO5300_COL_OFFSET 6`. Like every CO5300 it cannot rotate in hardware and needs even-aligned flush regions.
- **BOOT (GPIO 0) rotates the UI 90° CW per press** instead of HID Space: the quadrant is persisted to NVS and applied by the 2.16-style CPU strip rotation in `display.cpp`; touch coordinates map back through `rotation_map.h` (host-tested in `test/test_amoled_132_rotation`). So there is no push to talk. `has_rotation` is true but button-driven, not IMU-driven.
- PWR (GPIO 17, active LOW) keeps the shared role: short → brightness, hold ~3 s and release → pair.
