# ESP32-2432S028R (CYD) — `esp32_2432s028r`, `esp32_2432s028r_landscape`

Hardware notes for this board. Shared firmware rules are in `firmware/CLAUDE.md`.

Sunton ESP32-2432S028R, the "Cheap Yellow Display": classic ESP32-D0WD (dual-core Xtensa, 4 MB flash, **no PSRAM**), 2.8" ILI9341 240×320 over 4-wire SPI, XPT2046 *resistive* touch on its own SPI bus, CH340 USB-UART. Two envs share this folder: `esp32_2432s028r` (portrait, USB edge at the bottom) and `esp32_2432s028r_landscape` (`-DBOARD_LANDSCAPE`: 320×240, controller rotation 1, touch axes swapped).

- **Memory is tight**: no PSRAM (`BOARD_HAS_PSRAM` unset, so LVGL uses internal buffers and there is no `screenshot` command, like the C6 boards) and static DRAM is nearly full. The 4 MB flash uses `huge_app` (3 MB app, no OTA). Check the DRAM figure after any shared-code growth.
- Several silicon revisions ship under this name. The original micro-USB unit is ILI9341; later "R2"/USB-C units have been seen with ST7789 panels or a different init gamma (see `display.cpp`).
- The XPT2046 returns raw 12-bit counts that need per-unit calibration: `TP_RAW_*` are the community defaults; build with `-DTOUCH_DEBUG` to log raw samples and retune. Its X axis runs along the short edge, so landscape swaps axes (`TP_SWAP_XY`, `TP_INVERT_*`). Pressure gate `TP_PRESSURE_MIN`.
- Backlight is LEDC PWM on GPIO 21 (no brightness command). The display SPI is capped at 40 MHz (80 is unreliable on CYD wiring).
- **One user button, BOOT (GPIO 0), takes the PWR role** (`power.cpp`: short → brightness, hold ~1.5 s and release → pair) so pairing and brightness stay reachable with an uncalibrated touch panel. Cost: no HID Space (PTT). `button_count = 0`. The other key is a hardware RST.
- No battery circuit, no IMU. GPIO 26 drives an amplified speaker, but the chime engine is ES8311/I2S, so `sound.cpp` no-ops.
