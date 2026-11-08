# ESP32-2432S024C (CYD) — `esp32_2432s024c`, `esp32_2432s024c_landscape`

Hardware notes for this board. Shared firmware rules are in `firmware/CLAUDE.md`.

Sunton ESP32-2432S024C: classic ESP32-D0WD (4 MB flash, **no PSRAM**), 2.4" ST7789 240×320 over 4-wire SPI, **CST820 capacitive touch** on I2C (0x15; SDA 33, SCL 32, reset GPIO 25, no INT), CH340 USB-UART. Two envs share this folder: `esp32_2432s024c` (portrait) and `esp32_2432s024c_landscape` (`-DBOARD_LANDSCAPE`: 320×240, controller rotation 3).

- **Memory is tight**: no PSRAM (`BOARD_HAS_PSRAM` unset, so LVGL uses internal buffers and there is no `screenshot` command, like the C6 boards) and static DRAM is nearly full. The 4 MB flash uses `huge_app` (3 MB app, no OTA). Check the DRAM figure after any shared-code growth.
- The sibling of the resistive ESP32-2432S028R (same SPI pins 14/13/12/15/2), but the controller is an ST7789 and touch is capacitive: no calibration constants. Backlight is on GPIO 27 (LEDC PWM; no brightness command).
- **One user button, BOOT (GPIO 0), takes the PWR role** (`power.cpp`: short → brightness, hold ~1.5 s and release → pair). No HID Space (PTT); `button_count = 0`.
- No battery circuit, no IMU. No chime path (`sound.cpp` no-ops).
