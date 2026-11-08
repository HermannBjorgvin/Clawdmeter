# T-Display S3 — `lilygo_t_display_s3`

Hardware notes for this board. Shared firmware rules are in `firmware/CLAUDE.md`.

LILYGO T-Display S3 (Basic, **no touch**): ST7789 170×320 portrait IPS TFT on an 8-bit i80 parallel bus, ESP32-S3R8 (16 MB flash + 8 MB octal PSRAM). Build env: `lilygo_t_display_s3`.

- The panel rail (GPIO 15, HIGH = on) must be raised in `board_init()` before anything talks to the ST7789, and the bus read strobe (GPIO 9) is parked HIGH. Backlight is LEDC PWM on GPIO 38; the panel has no brightness command.
- The ST7789 RAM is 240 wide but only 170 columns reach the glass: `LCD_COL_OFFSET` 35.
- No PMU: the battery is read from an ADC divider on GPIO 4 (Vbat = pin × 2); charging and VBUS are not observable, so both report false.
- Two buttons: BOOT (GPIO 0, a strap pin: don't hold it through power-up) → Space/PTT, and GPIO 14 → PWR role (short → brightness, hold ~1.5 s and release → pair). With no touchscreen, a BOOT press first dismisses an active alert (`director_button`), then keeps its PTT role.
- No IMU, no audio.
