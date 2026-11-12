# LCD-2 — `waveshare_lcd_2`

Hardware notes for this board. Shared firmware rules are in `firmware/CLAUDE.md`.

Waveshare ESP32-S3-Touch-LCD-2 (ST7789T3 240×320 portrait IPS over 4-wire SPI, CST816D touch @ 0x15, QMI8658C IMU unused, ESP32-S3R8: 16 MB flash + 8 MB octal PSRAM). Build env: `waveshare_lcd_2`. SD card and the OV3660 camera are wired but unused.

- **LCD reset shares GPIO 0 with BOOT**: `gfx->begin()` pulses it low once, afterwards it stays high. BOOT (KEY2) can only act as a hardware reset and KEY1 is EN, so **no button reaches the firmware** (`button_count = 0`, `has_pwr_button = false`): navigation is touch only, and pairing is the 5 s touch hold on the pairing hint.
- Touch is polled (INT = GPIO 46, no reset pin: the chip self-resets at power-on). I2C is SDA 48 / SCL 47.
- No PMU: the battery is read from an ADC divider on GPIO 5 (R19 200K / R20 100K, the pin sees Vbat / 3). The charger status pin is not exposed, so "USB plugged in" (HWCDC) stands in for charging.
- Backlight is LEDC PWM on GPIO 1; the panel has no brightness command. No audio.
