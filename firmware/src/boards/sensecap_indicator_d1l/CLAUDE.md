# SenseCap Indicator D1L — `sensecap_indicator_d1l`

Hardware notes for this board. Shared firmware rules are in `firmware/CLAUDE.md`.

Seeed Studio SenseCap Indicator D1L (ST7701S 480×480 RGB parallel, FT6x36 touch @ **0x48**, PCA9535 IO expander @ 0x20, ESP32-S3 with octal PSRAM). Build env: `sensecap_indicator_d1l`. The RP2040 co-processor and its sensors are not used.

- The PCB routes ESP32-S3 GPIO n to LCD data bus D[15-n] (bit reversal), so the RGB pin list in `board.h` looks scrambled on purpose.
- The PCA9535 gates the panel's SPI chip select (P04) and the touch reset (P07). `io_expander_init()` runs before the panel and touch init; the ST7701S init is sent over 3-wire SPI (SCK 41, MOSI 48) with CS on the expander.
- Backlight is GPIO 45; the panel has no brightness command.
- **The only button is the GPIO 38 user key**, wired as the PWR role (short → brightness, hold ~1.5 s and release → pair). There is no BOOT/primary button, so `input_hal_is_held()` is always false and there is no push to talk.
- No PMU, battery, IMU or audio path.
