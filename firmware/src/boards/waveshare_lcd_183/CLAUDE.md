# LCD-1.83 — `waveshare_lcd_183`

Hardware notes for this board. Shared firmware rules are in `firmware/CLAUDE.md`.

Waveshare ESP32-S3-Touch-LCD-1.83 (ST7789P 240×284 over 4-wire SPI, CST816D touch @ 0x15, AXP2101 PMU, QMI8658 IMU). Build env: `waveshare_lcd_183`. Same I2C peripherals and SDA/SCL (15/14) as the AMOLED-1.8, but an SPI TFT and **no IO expander**.

- LCD reset (GPIO 38) is handed to `Arduino_ST7789`; the touch reset (GPIO 39) is released in `board_init()` before anything probes I2C.
- **Row offset 0, not Waveshare's demo value of 20**: on this panel the visible glass maps to GRAM rows 0..283, and offset 20 leaves the top rows as power-on garbage and clips the bottom.
- Backlight is a GPIO (LCD_BL 40, active HIGH), PWM-dimmed; the panel has no brightness command.
- PWR is the AXP2101 PKEY (short → brightness, hold → pair, same scheme as the AMOLED-2.16); BOOT (GPIO 0) is Space/PTT. No secondary button.
- QMI8658 is initialised for bus health, rotation is off (fixed portrait). No audio path in this port (`sound.cpp` no-ops).
