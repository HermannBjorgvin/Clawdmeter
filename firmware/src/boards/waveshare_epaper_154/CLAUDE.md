# ePaper-1.54 — `waveshare_epaper_154`

Hardware notes for this board. Shared firmware rules are in `firmware/CLAUDE.md`.

Waveshare ESP32-S3-ePaper-1.54 (SSD1681 200×200 black-and-white e-paper over SPI, ESP32-S3). Build env: `waveshare_epaper_154`. The first `minimal_ui` board: meters and a still corner Clawd, nothing animates (a refresh is slow and the panel is bistable).

- **`EPD_PWR` (GPIO 6) is active LOW and must be driven LOW before any SPI traffic**, or the SSD1681 has no Vcc, silently swallows every transaction, and the panel keeps the factory image.
- **Battery soft-power latch**: on battery the PWR side-button only momentarily applies the battery rail. `board_init()` must drive `VBAT_PWR_GPIO` (GPIO 17, active HIGH, the opposite polarity of `EPD_PWR`) HIGH before any display activity or power collapses when the button is released.
- Battery voltage is GPIO 4 (ADC1_CH3, radio-safe) behind a 1:2 divider: Vbat = pin mV × 2. GPIO 17 is the latch, not a sense line.
- **No touch, no PMU, no IMU, no audio.** BOOT (GPIO 0 → Space/PTT) and the PWR side-button (GPIO 18 → brightness, hold to pair). With no touchscreen, a BOOT press first dismisses an active alert (`director_button`), then keeps its PTT role.
- Pins come from Waveshare's `ESP32-S3-ePaper-1.54` example (`07_BATT_PWR_Test/user_config.h`).
