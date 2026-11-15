# LCD-2.1 — `waveshare_lcd_21` (and the LCD-2.8C variant, `waveshare_lcd_28c`)

Hardware notes for this board. Shared firmware rules are in `firmware/CLAUDE.md`.

This folder also builds the Waveshare ESP32-S3-Touch-LCD-2.8C: env `waveshare_lcd_28c` `extends` `waveshare_lcd_21` and adds `-DBOARD_LCD28C` (the `esp32_2432s024c_landscape` pattern). Its differences are under `#ifdef BOARD_LCD28C`; see "LCD-2.8C variant" below. With the flag off the image is the 2.1's (checked: byte-identical apart from the build timestamp and image hashes).

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

## LCD-2.8C variant (`BOARD_LCD28C`, env `waveshare_lcd_28c`)

Waveshare ESP32-S3-Touch-LCD-2.8C: the same wiring as above (pins, TCA9554 and its EXIO map, backlight, battery ADC, BOOT-only input, 480×480 round, the bit-banged init SPI). Pins are from Waveshare's wiki and ESP-IDF demo (`ESP32-S3-Touch-LCD-2.8C-Demo.zip`); they agree. Only the touch 2.8C is supported: the plain 2.8 has no touch. The 2.8B is a rectangular panel, so it would be its own env (`waveshare_lcd_28b`), not a variant here. Folding the 2.1 and 2.8C into one boot-detected image, with the app still listing two boards, is specified in `AGENT-SPEC-lcd21-28c-one-image.md` (repo root).

What the flag changes:

- Name and logs: `BOARD_NAME` "Waveshare LCD 2.8C", log tag `LCD-2.8C` (`BOARD_LOG_TAG`).
- Init table: `st7701_init_28c.h` (`st7701_lcd28_init_operations`), the demo's "2.8inch" `ST7701S_screen_init`, checked step for step (commands, data, delays) against the demo. Different power, gamma and GIP values; starts with the bank-0x13 `EF 08` write and ends Sleep Out → 120 ms → COLMOD 0x66 → MADCTL 0x00 → TE On → Display On, with no Inversion Off.
- Porches: vsync 8/2/18 (`LCD_VSYNC_PULSE` / `LCD_VSYNC_BACK`); hsync 50/8/10 and the 12 MHz PCLK as on the 2.1.
- Touch: **GT911** via SensorLib (`TouchDrvGT911`), reset on EXIO2, INT on GPIO 16, polled, no swap or mirror (the demo's settings). It latches its I2C address from INT on the rising edge of reset, so `board_init()` asserts both resets, then drives INT low (datasheet order; the demo drives INT first, contending with the running controller), releases both resets together, holds INT low 10 ms for 0x5D, releases it and waits 50 ms: ~70 ms against the demo's 350 ms. With no pins set, SensorLib's `begin()` probes 0x5D then 0x14 by itself; the log prints `getAddress()`, the address it settled on.
- Battery: `BAT_VOLT_DIVIDER` is 3 / 0.980952, as the demo's `BAT_Driver` computes it (`mV * 3 / Measurement_offset`): the calibration raises the reading by about 2 %. The charger is unnamed on the wiki; the other key is the battery power control.

Hardware status, one ESP32-S3-Touch-LCD-2.8C (built as its own folder before it became this variant; same code): boot in 683 ms, GT911 at 0x5D, BLE pairing with the app and live usage, meters and pairing hint inside the circle (framebuffer screenshot), touch at centre and all four edges (centre ≈ (240, 260), top (314, 25), right (416, 242), left (45, 257)), no phantom touches over 25 s, hold-to-pair. With no cell fitted BAT reads about 4.2 V (also in Waveshare's factory firmware). That unit's panel has a hardware defect (a grid of wrongly lit tracks and dark patches, also under the factory firmware), so image quality, colours and the 12 MHz PCLK aren't verified on good glass. Battery not tested with a cell fitted.
