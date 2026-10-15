# LCD-4 — `waveshare_lcd_4`

Hardware notes for this board. Shared firmware rules are in `firmware/CLAUDE.md`.

Waveshare ESP32-S3-Touch-LCD-4 (ST7701 RGB parallel, 480×480 square, GT911 touch). Build env: `waveshare_lcd_4`. **RGB-panel port**: Arduino_ESP32RGBPanel + bounce buffers (tearing fix). IO expander @ 0x24 (TCA9554 / CH32V003) must init before `gfx->begin()` or the panel stays dark; backlight is expander pin 2 (on/off only). No AXP2101 / IMU; KEY/PWR is hardware RST. Single BOOT button (GPIO 0 → Space/PTT).

## Hardware (critical pins)

- Display: **ST7701** 480×480 RGB parallel (DE=40, VSYNC=39, HSYNC=38, PCLK=41, R0-4=46/3/8/18/17, G0-5=14/13/12/11/10/9, B0-4=5/45/48/47/21); ST7701 init via SW SPI (CS=42, SCK=2, MOSI=1).
- Touch: **GT911** via I2C (SDA=15, SCL=7), polled (wiki INT=GPIO 16 unused). Probe 0x5D then 0x14.
- IO expander: **addr 0x24** (fallback 0x20) on the same I2C bus — must init before `gfx->begin()` (output 0xFF, config 0x3A). Backlight is expander pin 2.
- No PMU / IMU. Buttons: GPIO 0 only (BOOT → Space/PTT). KEY/PWR is EN/RST (hardware reset). GPIO 18 is display R3.
- No PWR button (`has_pwr_button = false`): the pairing hint says to pair from the computer, and a 5 s touch hold on it clears the bonds (`pair_hold.{h,cpp}`, native-tested; `SIM_NO_PWR=1` in the sim). The `unpair` serial command (reply `{"unpair":true}`) clears them on any board.
- RGB tearing fix: pass `bounce_buffer_size_px = LCD_WIDTH * 10` to `Arduino_ESP32RGBPanel`. Do not call `rgbpanel->getFrameBuffer()` after `gfx->begin()`.
- Only one user button, so: no brightness cycling, re-pairing is the 5 s touch hold on the pairing hint, and shows only come from situations.
