# ESP32-S3 4848S040 — `esp32_s3_4848s040`

Hardware notes for this board. Shared firmware rules are in `firmware/CLAUDE.md`.

Generic "Guition" ESP32-S3 4848S040 (ST7701S 480×480 RGB parallel, GT911 touch, ESP32-S3 with PSRAM). Build env: `esp32_s3_4848s040`. The RGB scan-out reads a PSRAM framebuffer, so every LVGL flush ends in a cache writeback (`Cache_WriteBack_Addr` in `display.cpp`).

- The ST7701S init sequence is the OpenHASP one, sent over 3-wire SPI (CS 39, SCLK 48, MOSI 47); pixel data uses the 16-bit RGB bus. The panel has no brightness command: the backlight is PWM on GPIO 38.
- Touch is a GT911 on I2C (SDA 19, SCL 45) with **no INT or RST pin wired**: both addresses (0x5D, 0x14) are probed at init and the controller is polled.
- **No readable buttons.** BOOT and RST are physically inaccessible, GPIO 0 is display R4, and the front SW1 is wired to the IP5306's KEY pin. Touch is the only input, so `button_count = 0` and `has_pwr_button = false`: re-pairing is the 5 s touch hold on the pairing hint, and an alert is dismissed with a tap.
- An IP5306 power-bank PMIC is fitted but unread: no battery indicator, and `power_hal_is_vbus_in()` returns true so the idle fade doesn't sleep the panel.
- No IMU, no audio.
