# AMOLED-1.43 — `waveshare_amoled_143`

Hardware notes for this board. Shared firmware rules are in `firmware/CLAUDE.md`.

Waveshare ESP32-S3-Touch-AMOLED-1.43 (466×466 **round** AMOLED over QSPI, 16 MB flash + 8 MB octal PSRAM). Build env: `waveshare_amoled_143`. Round: `round_panel = true`, so the meters and Clawd's entrances follow the circle (`layout.cpp`, `docs/clawd-director.md`).

- **Two panel revisions, detected at boot** (`board_rev()` in `board_init.cpp`, enum in `board_rev.h`), by which touch address answers: early = SH8601 display + FT3168 touch (0x38); current = CO5300 display + CST820 touch (0x15, shares the CST816 reader). One binary drives both; `display.cpp` picks the matching GFX driver.
- The display reset is a direct GPIO (21), so there is no IO expander step in `board_init()`. Touch is polled (no INT pin wired); I2C is SDA 47 / SCL 48. The IMU and RTC sit on a separate bus the firmware doesn't use.
- **No PWR button reaches the firmware** (the other key is a hardware reset) and BOOT (GPIO 0) is push to talk, so `has_pwr_button = false`: pairing is cleared with the 5 s touch hold on the pairing hint. No battery or fuel gauge, no audio.
