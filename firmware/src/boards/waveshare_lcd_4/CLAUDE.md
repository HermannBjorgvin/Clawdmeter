# LCD-4 — `waveshare_lcd_4`

Hardware notes for this board. Shared firmware rules are in `firmware/CLAUDE.md`.

Waveshare ESP32-S3-Touch-LCD-4 (ST7701 RGB parallel, 480×480 square, GT911 touch). Build env: `waveshare_lcd_4`. **RGB-panel port**: Arduino_ESP32RGBPanel + bounce buffers (tearing fix). IO expander must init before `gfx->begin()` or the panel stays dark; the chip differs by board revision (below). Optional LiPo with battery % on the CH32V003 revision. No AXP2101 / IMU; KEY/PWR is hardware RST. Single BOOT button (GPIO 0 → Space/PTT).

## Hardware (critical pins)

- Display: **ST7701** 480×480 RGB parallel (DE=40, VSYNC=39, HSYNC=38, PCLK=41, R0-4=46/3/8/18/17, G0-5=14/13/12/11/10/9, B0-4=5/45/48/47/21); ST7701 init via SW SPI (CS=42, SCK=2, MOSI=1).
- Touch: **GT911** via I2C (SDA=15, SCL=7), polled (wiki INT=GPIO 16 unused). Probe 0x5D then 0x14.
- IO expander, picked by address in `io_expander.cpp`:
  - **CH32V003 @ 0x24** (V4.0 boards). Register/pin map from Waveshare's `WS_CH32_IO` library (`waveshareteam/ESP32-S3-Touch-LCD-4`): 0x02 direction (1 = output), 0x03 output, 0x05 backlight PWM (0–255, **inverted**: higher = dimmer; `io_expander_set_brightness()` writes `255 - level`), 0x06 battery ADC (10-bit LE, V = raw·3.3/1023·3). Bits: 0 charger STAT, 1 touch RST, 3 LCD RST, 5 SYS_EN (battery power-hold latch, keep high), **6 buzzer**. **Keep every pin an output**: making EXIO0 (STAT) an input sounds the buzzer continuously. The CH32 does **not** reset with the ESP32: a reset mid-transaction can wedge the bus and latch the buzzer on until a power cycle, so `board_init()` clocks the bus free first and every write retries.
  - **TCA9554 @ 0x20** (V1–V3). Pin map from Waveshare's V1 and V2/V3 demo code: EXIO0 TP_RST, 1 BL_EN (on/off only), 2 LCD_RST, 3 SD_CS, 4 BLC, 5 buzzer (HIGH = on), 6 RTC_INT. No battery ADC, so `has_battery` is false at runtime (`caps.cpp`). These boards have an SW6106 power-bank chip at 0x3C whose light-load shutdown `power.cpp` disables (Waveshare FAQ). Not tested on hardware.
- Battery (`power.cpp`, CH32 only): no charger status is readable, so "on USB" = `HWCDC::isPlugged()` or a rising voltage trend. Voltage reads high while charging, so the charging % starts from the last on-battery % (saved in NVS, since the board resets on plug/unplug) and ramps 90→100 in the constant-voltage phase. On battery the backlight is capped at 35 %. Plugging/unplugging USB with a battery fitted resets the V4.0 board (hardware hold-up gap); press PWR after unplugging to run on battery.
- Buzzer (`sound.cpp`): active, fixed tone, so `sound_hal_chime` plays three short beeps; a failed "off" write is retried every tick.
- No PMU / IMU. Buttons: GPIO 0 only (BOOT → Space/PTT). KEY/PWR is EN/RST (hardware reset). GPIO 18 is display R3.
- No PWR button (`has_pwr_button = false`): the pairing hint says to pair from the computer, and a 5 s touch hold on it clears the bonds (`pair_hold.{h,cpp}`, native-tested; `SIM_NO_PWR=1` in the sim). The `unpair` serial command (reply `{"unpair":true}`) clears them on any board.
- Fixed UI rotation via `LCD_ROTATION` in `board.h` (quarter turns CW; 0 = native, USB port on the left; 1 = USB at the bottom). `display.cpp` remaps each LVGL strip into a scratch buffer and blits at native orientation (GFX's `setRotation` path falls back to per-pixel writes); `touch.cpp` applies the inverse map. Screenshots capture the LVGL buffer, so they're always upright.
- RGB tearing fix: pass `bounce_buffer_size_px = LCD_WIDTH * 10` to `Arduino_ESP32RGBPanel`. Do not call `rgbpanel->getFrameBuffer()` after `gfx->begin()`.
- Only one user button, so: no brightness cycling, re-pairing is the 5 s touch hold on the pairing hint, and shows only come from situations.
