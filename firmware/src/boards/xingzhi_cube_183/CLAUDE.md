# Xingzhi Cube 1.83 — `xingzhi_cube_183`

Hardware notes for this board. Shared firmware rules are in `firmware/CLAUDE.md`.

Xingzhi Cube 1.83" TFT WiFi (284×240 landscape SPI TFT, ESP32-S3 with 16 MB flash and 8 MB octal PSRAM, three user buttons). Build env: `xingzhi_cube_183`. Both the one-mic and the two-mic variant (the former #31) use this env.

- **Sold as ST7789 but needs a vendor init table** (NV3023-style registers 0xFD..0xF6, BGR, inversion on, swap_xy + mirror_y, 36-column offset), driven through `Arduino_ST7789` with the table as its init operations. Backlight is LEDC PWM on GPIO 13.
- **GPIO 21 is the power-hold latch**: `board_init()` drives it HIGH or the board switches off when USB is removed.
- Battery: a bare Li-Po on a divider into GPIO 17 (ADC2_CH6, fine because only BLE is used), raw 1970 (empty) .. 2430 (full); GPIO 38 is the charger's CHRG pin (HIGH = charging). VBUS is reported present.
- **No touchscreen, no PMU, no IMU, no audio path.** Buttons (active LOW): BOOT (GPIO 0 → Space/PTT), VOL (GPIO 40 → Shift+Tab), MID (GPIO 39 → PWR role: brightness, hold ~1.5 s and release to pair). With no touch, a BOOT press first dismisses an active alert (`director_button`), then keeps its PTT role.
