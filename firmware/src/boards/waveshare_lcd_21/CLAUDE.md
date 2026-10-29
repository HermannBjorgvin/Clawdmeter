# LCD-2.1 — `waveshare_lcd_21`

Hardware notes for this board. Shared firmware rules are in `firmware/CLAUDE.md`.

Waveshare ESP32-S3-Touch-LCD-2.1 (ST7701S RGB parallel, 480×480 **round**, CST820 touch). The 2.1B is the same board with 2.5D curved cover glass; one env serves both. Build env: `waveshare_lcd_21`. **RGB-panel port** on the LCD-4 scheme: Arduino_ESP32RGBPanel + bounce buffers, PSRAM frame buffer. Module is ESP32-S3R8 (8 MB OPI PSRAM) with 16 MB flash. Pins are from Waveshare's schematic and ESP-IDF demo; they agree.

## Hardware (critical pins)

- Display: **ST7701S** 480×480 RGB parallel (DE=40, VSYNC=39, HSYNC=38, PCLK=41, R0-4=46/3/8/18/17, G0-5=14/13/12/11/10/9, B0-4=5/45/48/47/21). The schematic numbers R and B from 1 (R0/B0 NC). Porches hsync front/pulse/back 50/8/10, vsync 8/3/8 (demo values). PCLK is the library default 12 MHz (LCD-4's), not the demo's 18 MHz: the frame buffer streams from PSRAM and flash writes (NVS prefs, BLE bonds) stall the cache, so a faster scan risks bounce-buffer underruns.
- ST7701S init: 9-bit 3-wire SPI on SCK=2, MOSI=1 (shared with the SD card's SCLK/MOSI, 10k pull-ups). **CS is expander EXIO3**. **Don't use `Arduino_SWSPI`**: it clocks at full register speed and this panel ignores every command (black screen, backlight on; seen on hardware). `display.cpp` bit-bangs the table itself at ~500 kHz with EXIO3 held low for the whole sequence, as the demo does (4 MHz), and builds `Arduino_RGB_Display` with no bus so `begin()` only starts the RGB panel.
- Init table: `st7701_init.h`, transcribed from the demo. Arduino_GFX's `st7701_type7` has the same gamma/GIP values but a different ending (no 480 ms Sleep Out wait, no Inversion Off). `DELAY` takes one byte; long waits are split.
- Backlight: GPIO 6 drives an N-FET, high = on. LEDC PWM 5 kHz/8-bit, so brightness and the idle fade dim smoothly (unlike LCD-4's on/off expander pin).
- I2C (SDA=15, SCL=7): CST820 @ 0x15, TCA9554 @ 0x20, QMI8658 @ 0x6B, PCF85063 RTC @ 0x51.
- **TCA9554 @ 0x20** (A0-A2 low). EXIO1 LCD_RST, EXIO2 TP_RST, EXIO3 LCD_CS, EXIO4 SD_CS, EXIO5/6 IMU INT2/INT1, EXIO7 RTC INT, EXIO8 buzzer. `board_init()` writes the latch (resets released, LCD and SD CS high, buzzer low) before the config (EXIO5-7 inputs), then pulses both resets. It must run before `display_hal_begin()` or the panel and touch stay in reset. The probe retries once; failed writes are logged.
- Touch: **CST820** @ 0x15, INT=GPIO 16 (falling), reset on EXIO2. No swap or mirror.
- Battery: ETA6098 standalone charger (no I2C). BAT_ADC on GPIO 4 behind a 200k/100k divider (×3). Charging is unknowable. VBUS is always reported present (like the LCD-4) so USB desk use never idle-sleeps; on battery the display doesn't idle-sleep either.
- Buttons: BOOT (GPIO 0) only. RESET is EN; SW1 is the battery power slide switch. No PWR role (`has_pwr_button = false`): re-pairing is the 5 s touch hold on the pairing hint, or the `unpair` serial command.
- Buzzer: active, on/off via EXIO8. It can't play the ding, so `sound.cpp` is a no-op and the latch keeps it off.
- QMI8658 is populated but unused: no CPU-rotation path for the RGB panel yet (`BOARD_HAS_ROTATION 0`).

## Hardware status

Seen working on an ESP32-S3-Touch-LCD-2.1: boot, BLE, the panel, the meters drawn inside the circle, touch (CST820, no swap or mirror needed) including hold-to-pair, buzzer silent at boot. Battery not tested with a cell. With none fitted the icon reads nearly full: the charger holds BAT near 4.2 V, so the ADC can't tell "no battery" from "full" (the host gets ~100 % too).

The CST820 didn't ACK its ID read when touch init ran ~6 s after reset (a debug delay); at the normal ~1 s it answers (ID 0xB7). It likely sleeps when idle.
