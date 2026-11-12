# ePaper-1.54 — `waveshare_epaper_154`

Hardware notes for this board. Shared firmware rules are in `firmware/CLAUDE.md`.

Waveshare ESP32-S3-ePaper-1.54 (SSD1681 200×200 black-and-white e-paper over SPI, ESP32-S3). Build env: `waveshare_epaper_154`. The first `minimal_ui` board: meters and a still corner Clawd, nothing animates (a refresh is slow and the panel is bistable).

- **`EPD_PWR` (GPIO 6) is active LOW and must be driven LOW before any SPI traffic**, or the SSD1681 has no Vcc, silently swallows every transaction, and the panel keeps the factory image.
- **Battery soft-power latch**: on battery the PWR side-button only momentarily applies the battery rail. `board_init()` must drive `VBAT_PWR_GPIO` (GPIO 17, active HIGH, the opposite polarity of `EPD_PWR`) HIGH before any display activity or power collapses when the button is released.
- Battery voltage is GPIO 4 (ADC1_CH3, radio-safe) behind a 1:2 divider: Vbat = pin mV × 2. GPIO 17 is the latch, not a sense line.
- **Keep redraws to state changes.** Every change on screen is a partial refresh that blocks `loop()` for about 300 ms (`display.cpp`: at most one a second, and the first change after 10 min is a full refresh that clears ghosting). The title clock, once a minute, is the only timed redraw; minimal mode's status line changes only with the state. Anything periodic costs battery, panel wear and button latency.
- **1 bit, `mono_panel`**: `display.cpp` inks every pixel at luminance 64 or more (`hal/mono_panel.h`), so the board keeps the default palette and the meters draw outlined bars, inverted pills and an undimmed stale view (`docs/porting/capability-flags.md`). Preview it in the simulator at 200×200 with `SIM_MINIMAL=1 SIM_MONO=1 SIM_NO_TOUCH=1 SIM_BUTTONS=1`, or the sim grid's `epaper` cell.
- **No touch, no PMU, no IMU, no audio.** BOOT (GPIO 0 → Space/PTT) and the PWR side-button (GPIO 18 → brightness, hold to pair). With no touchscreen, a BOOT press first dismisses an active alert (`director_button`), then keeps its PTT role.
- Pins come from Waveshare's `ESP32-S3-ePaper-1.54` example (`07_BATT_PWR_Test/user_config.h`).
