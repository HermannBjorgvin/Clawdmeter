# SpotPear S3 LCD-1.54 — `spotpear_s3_154`

Hardware notes for this board. Shared firmware rules are in `firmware/CLAUDE.md`.

SpotPear ESP32-S3-LCD-1.54 "MUMA" (ST7789 240×240 over 4-wire SPI, CST816 touch @ 0x15, ESP32-S3 N16R8: 16 MB flash + 8 MB octal PSRAM). Build env: `spotpear_s3_154`.

- **Not the Waveshare LCD-1.54 pin map**, despite SpotPear's wiki linking to it. Pins come from the xiaozhi-esp32 `sp-esp32-s3-1.54-muma` config; touch SDA=11 / SCL=7 were confirmed with an I2C scan.
- Backlight is LEDC PWM on GPIO 42 and **active LOW** (`LCD_BL_INVERT`); the panel has no brightness command.
- No PMU or fuel gauge: no battery indicator (`BOARD_HAS_BATTERY 0`), no VBUS/charging state.
- One user button wired, BOOT (GPIO 0 → Space/PTT). The RST key is EN (hardware reset), so `has_pwr_button = false`: re-pairing is the 5 s touch hold on the pairing hint. A "PLUS" button exists but is unwired.
- QMI8658 is populated but unused (fixed orientation); `sound.cpp` is a no-op.
