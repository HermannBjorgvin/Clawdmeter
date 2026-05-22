# Xingzhi Cube 1.83" TFT WiFi (2mic) — port

## Status: working ✓ (high-contrast monochrome render)

The firmware boots, advertises BLE, drives the NV3023 panel, and
renders the LVGL UI at correct geometry. Because this specific panel
exhibits stubborn color polarity quirks that standard `INVOFF` /
`MADCTL` knobs don't fix, `display_hal_draw_bitmap` runs every pixel
through a per-pixel monochrome threshold: bright pixels resolve to
white, dark pixels to black, with the final output pre-inverted to
land white-on-panel where LVGL wrote a bright color. The result is a
crisp black-and-white silhouette — Clawd reads cleanly as white-on-
black and all text stays sharp.

To experiment with native (slightly-off) colors instead, comment out
`#define MONOCHROME_RENDER` in `display.cpp`.

## Key finding: panel is NV3023, not ST7789

Every reference to ST7789 on the board's silkscreen / forum threads is
misleading. The actual driver is **NV3023**. ST7789 init sequences
(both Arduino_GFX and TFT_eSPI) silently fail. The init sequence here
was transcribed from the xiaozhi-esphome project:

<https://github.com/RealDeco/xiaozhi-esphome/blob/main/devices/Xingzhi/xingzhi-cube-1.83-2mic.yaml>

## Final pixel-format config

After empirical testing, the combination that gives correct colors for
both `fillScreen` and LVGL anti-aliased rendering is:

| Setting             | Value                            |
|---------------------|----------------------------------|
| LVGL color format   | `LV_COLOR_FORMAT_RGB565_SWAPPED` |
| MADCTL (0x36)       | `0xA0` (MY + MV, no BGR bit)     |
| INVOFF (0x20)       | sent explicitly                  |
| COLMOD (0x3A)       | `0x55` (RGB565)                  |
| Column offset       | 36                               |
| Render mode         | Monochrome threshold + invert    |

LVGL's SWAPPED format writes pixels in MSB-first byte order, which
matches the byte order the NV3023 expects over SPI. `draw_bitmap`
streams the buffer verbatim — no byte swap, no channel swap on the
CPU side.

## Pin map

```
SPI:        MOSI=10, SCLK=9, DC=8, CS=14, RST=18, BL=13
Buttons:    BOOT=GPIO0 (primary/Space)
            VOL_DOWN=GPIO40 (secondary/Shift+Tab)
            VOL_UP=GPIO39 (cycle screens)
Battery:    ADC=GPIO17, CHRG=GPIO38
Power latch:GPIO21 (drive HIGH to stay on)
Backlight:  GPIO13 via LEDC PWM (NOT plain digitalWrite — the panel
            has an LDO/filter on BL that needs PWM)
```

## Build / flash

```bash
~/.platformio/penv/bin/pio run -d firmware -e xingzhi_cube_183
~/.platformio/penv/bin/pio run -d firmware -e xingzhi_cube_183 \
    -t upload --upload-port /dev/cu.usbmodemXXXX
```

If the device gets stuck (boot loop crashes USB-CDC enumeration), hold
BOOT (GPIO0) while plugging in USB to enter ROM download mode.

## What works

- ✓ ESP32-S3-N16R8 boots, USB-CDC enumerates
- ✓ BLE advertises with device MAC
- ✓ NV3023 init sequence sent, display lit
- ✓ Backlight PWM via GPIO13 / LEDC
- ✓ LVGL splash renders Clawd at correct size/position
- ✓ Primary R/G/B colors display correctly
- ✓ Anti-aliased pixels recognizable (warm hues acceptable)
- ✓ BoardCaps reports `Xingzhi Cube 1.83, 284x240`

## Not yet verified on hardware

- Buttons (GPIO 0 / 39 / 40) — only verified by code path
- Battery ADC (GPIO17) — calibration values from xiaozhi config
- Power-hold latch (GPIO21) — drives HIGH in `board_init()`

## HAL extension added

This port required adding `display_hal_lv_color_format()` to the HAL
contract — boards that need pixel byte-order swap return
`LV_COLOR_FORMAT_RGB565_SWAPPED`; everyone else returns
`LV_COLOR_FORMAT_RGB565`. See `hal/display_hal.h`.
