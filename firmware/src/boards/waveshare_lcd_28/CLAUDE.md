# LCD-2.8C — `waveshare_lcd_28`

Hardware notes for this board. Shared firmware rules are in `firmware/CLAUDE.md`.

Waveshare ESP32-S3-Touch-LCD-2.8C (ST7701S RGB parallel, 480×480 **round**, GT911 touch). Build env: `waveshare_lcd_28`. Started as a copy of `waveshare_lcd_21`: the pins, expander, backlight and battery wiring are the same, while the panel init table, vsync porches and touch controller differ. **RGB-panel port** on the LCD-4 scheme: Arduino_ESP32RGBPanel + bounce buffers, PSRAM frame buffer. Module is ESP32-S3R8 (8 MB OPI PSRAM) with 16 MB flash. Pins are from Waveshare's wiki and ESP-IDF demo (`ESP32-S3-Touch-LCD-2.8C-Demo.zip`); they agree.

## Hardware (critical pins)

- Display: **ST7701S** 480×480 RGB parallel (DE=40, VSYNC=39, HSYNC=38, PCLK=41, R0-4=46/3/8/18/17, G0-5=14/13/12/11/10/9, B0-4=5/45/48/47/21). The wiki numbers R and B from 1 (R0/B0 NC). Porches: hsync front/pulse/back 50/8/10, vsync 8/2/18 (demo values; the LCD-2.1 uses vsync 8/3/8). PCLK is the library default 12 MHz (LCD-4's), not the demo's 18 MHz: the frame buffer streams from PSRAM and flash writes (NVS prefs, BLE bonds) stall the cache, so a faster scan risks bounce-buffer underruns.
- ST7701S init: 9-bit 3-wire SPI on SCK=2, MOSI=1 (shared with the SD card's SCLK/MOSI). **CS is expander EXIO3**. **Don't use `Arduino_SWSPI`**: on the LCD-2.1 the panel ignored every command at its full register speed. `display.cpp` bit-bangs the table at ~500 kHz with EXIO3 held low for the whole sequence, as the demo does (4 MHz), and builds `Arduino_RGB_Display` with no bus so `begin()` only starts the RGB panel.
- Init table: `st7701_init.h`, transcribed from the demo's `ST7701S_screen_init` ("2.8inch"). It is not the LCD-2.1 table: it differs in the power, gamma and GIP values, starts with the bank-0x13 `EF 08` write, and ends Sleep Out → 120 ms → COLMOD 0x66 → MADCTL 0x00 → TE On → Display On, with no Inversion Off.
- Backlight: GPIO 6, high = on. LEDC PWM 5 kHz/8-bit, as on the LCD-2.1 (the demo also drives it with LEDC).
- I2C (SDA=15, SCL=7): GT911 @ 0x5D, TCA9554 @ 0x20, QMI8658 and PCF85063 RTC (same addresses as the LCD-2.1 assumed, unused).
- **TCA9554 @ 0x20** (A0-A2 low). EXIO1 LCD_RST, EXIO2 TP_RST, EXIO3 LCD_CS, EXIO4 SD_CS, EXIO5/6 IMU INT2/INT1, EXIO7 RTC INT, EXIO8 buzzer. `io_expander_init()` writes the latch (resets released, LCD and SD CS high, buzzer low) before the config (EXIO5-7 inputs). `board_init()` then pulses both resets and must run before `display_hal_begin()`, or the panel and touch stay in reset.
- Touch: **GT911** via SensorLib (`TouchDrvGT911`), reset on EXIO2, INT on GPIO 16. The GT911 latches its address from INT on the rising edge of reset, so `board_init()` drives INT low through the reset (the demo's `touch_gt911_reset` sequence) for 0x5D, then releases INT to an input. `touch.cpp` falls back to 0x14 if 0x5D doesn't ACK. The controller is polled; no swap or mirror (the demo's settings).
- Battery: standalone charger (unnamed on the wiki; no I2C). BAT_ADC on GPIO 4, ×3 divider (the demo's `BAT_Driver` uses ×3 with a 0.98 calibration factor). Charging is unknowable; VBUS is always reported present, as on the LCD-2.1.
- Buttons: BOOT (GPIO 0) only. RESET is EN; the other key is the battery power control. No PWR role (`has_pwr_button = false`): re-pairing is the 5 s touch hold on the pairing hint, or the `unpair` serial command.
- Buzzer: on/off via EXIO8. It can't play the ding, so `sound.cpp` is a no-op and the latch keeps it off.
- QMI8658 is populated but unused (`BOARD_HAS_ROTATION 0`).

## Hardware status

Seen on one ESP32-S3-Touch-LCD-2.8C: boot in ~0.9 s, the expander, GT911 answering at 0x5D, BLE advertising, the meters and pairing hint drawn inside the circle (framebuffer screenshot). Touch checked by tapping centre and all four edges: centre ≈ (240, 260), top (314, 25), right (416, 242), left (45, 257), so no swap or mirror. No phantom touches over 25 s untouched.

That unit's panel has a hardware defect (a grid of wrongly lit tracks and repeating dark patches, worst on dark screens) that also shows under Waveshare's factory firmware, so the image quality, colours and the 12 MHz PCLK choice are not verified on good glass. Battery not tested.
