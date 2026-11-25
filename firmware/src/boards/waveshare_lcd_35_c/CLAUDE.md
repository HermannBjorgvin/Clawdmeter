# LCD-3.5-C — `waveshare_lcd_35_c`

Hardware notes for this board. Shared firmware rules are in `firmware/CLAUDE.md`.

Waveshare ESP32-S3-Touch-LCD-3.5-C (ST7796 320×480 over 4-wire SPI, FT6336 touch @ 0x38, AXP2101 PMU, QMI8658 IMU, ES8311 codec, TCA9554 IO expander @ 0x20). Build env: `waveshare_lcd_35_c`. The -C is the cased kit with an OV5640 camera (unused); the bare ESP32-S3-Touch-LCD-3.5 is the same board.

- **The first I2C transaction after `Wire.begin()` fails** (`ESP_ERR_INVALID_STATE`), so `io_expander_init()` writes and reads back each register with retries. Without that, the direction write is lost: EXIO1/EXIO7 stay inputs, the panel runs only on its reset pull-up and the amp is never switched.
- TCA9554 EXIO1 is the ST7796 reset; `board_init()` releases it before `display_hal_init()`. EXIO7 is the NS4150B amp CTRL (high = on), on only while a ding plays.
- Display: MOSI 1, SCLK 5, DC 3, backlight 6 (LEDC PWM; the panel has no brightness command). CS and reset are not GPIOs.
- **The ST7796 needs SPI mode 0.** Arduino GFX's ESP32 default (mode 3) leaves the panel backlit but ignoring commands.
- The FT6336 has no INT or reset wire; `touch_hal_read()` polls it.
- PWR is the AXP2101 PKEY (short → brightness, hold → pair, same scheme as the AMOLED-2.16); BOOT (GPIO 0) is Space/PTT. No secondary button.
- Rotation: QMI8658 `ax` ≈ -1 g upright, ≈ +1 g upside down. Only 0 and 180° are used; the ST7796 flips via MADCTL so the frame stays 320×480, and `touch_hal_read()` mirrors touch to match.
- Audio: ES8311 MCLK/BCLK/WS/DOUT/DIN on GPIO 12/13/15/16/14, codec volume 70. `power_hal_init()` sets ALDO1 to 3.3 V before `sound_hal_init()` probes the codec.
