# Freenove S3 Display (non-touch) — `freenove_s3_display_nontouch`

Hardware notes for this board. Shared firmware rules are in `firmware/CLAUDE.md`.

Freenove ESP32-S3 Display, non-touch version **FNK0104A** (2.8" ILI9341 240×320 portrait over plain 4-wire SPI, ESP32-S3R8: 16 MB flash + 8 MB octal PSRAM). Build env: `freenove_s3_display_nontouch`. The touch variant (FNK0104B, FT6336 on I2C) would need its own board folder.

- ILI9341 with inversion on (`ips = true`); the ST7789 init leaves this panel dark with a lit backlight. No reset line (software reset). The panel has no brightness command: the backlight is LEDC PWM on GPIO 45.
- **No touch, no PMU, no IMU, no audio.** `minimal_ui = true`: meters and a still corner Clawd only (slow SPI TFT).
- **BOOT (GPIO 0) is the only input**, so it takes the PWR role (`power.cpp`): short press → brightness, hold ~1.5 s and release → pair. `button_count = 0` (no primary/secondary: no push to talk). With no touchscreen, a short press first dismisses an active alert (`director_button`), then does its normal job.
