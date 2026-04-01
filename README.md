# Claude Usage Tracker - Panlee SC01 Plus

Displays your Claude Code usage limits on a Panlee SC01 Plus (ESP32-S3) 3.5" touchscreen.

![Demo](assets/demo.gif)

Shows your 5-hour session and 7-day weekly utilization.

## Hardware

- [Panlee SC01 Plus](http://en.smartpanle.com/product-item-15.html) (WT32-SC01 Plus) — ESP32-S3, 3.5" 480x320 IPS, capacitive touch
- USB-C cable for power and data)

## Prerequisites

- Linux (tested on my Ubuntu laptop)
- [PlatformIO CLI](https://docs.platformio.org/en/latest/core/installation/index.html)
- `curl`, `awk`, `inotify-tools` (pre-installed on most Linux distros, install `inotify-tools` if missing)
- Claude Code
- User in `dialout` group: `sudo adduser $USER dialout` (log out/in after)

## Flash the firmware

```bash
cd firmware
pio run -t upload
```

The SC01 Plus appears at `/dev/ttyACM0`.

## Run the daemon

The daemon polls your Claude usage every 30 seconds and sends it to the display over USB serial.

```bash
bash daemon/claude-usage-daemon.sh
```

## Install the systemd service

To auto-start the daemon on login:

```bash
./install.sh
```

Check status: `systemctl --user status claude-usage-daemon`

## How it works

1. The daemon reads your Claude Code OAuth token from `~/.claude/.credentials.json`
2. Makes a minimal API call to `api.anthropic.com/v1/messages` (1 token of Haiku, essentially free)
3. Extracts usage data from the response headers (`anthropic-ratelimit-unified-5h-utilization`, etc.)
4. Sends a JSON line over USB serial to the ESP32
5. The ESP32 parses it and updates the LVGL dashboard

## Recompiling fonts

The `firmware/src/font_*.c` files are pre-compiled LVGL bitmap fonts. If you need to regenerate them (e.g. to change sizes or add characters):

```bash
npm install -g lv_font_conv
```

Generate with `--no-compress` (required for LVGL 9):

```bash
# Tiempos Text (title)
lv_font_conv --font assets/TiemposText-400-Regular.otf -r 0x20-0x7E \
  --size 34 --format lvgl --bpp 4 --no-compress \
  -o firmware/src/font_tiempos_34.c --lv-include "lvgl.h"

# Styrene B (UI text)
lv_font_conv --font assets/StyreneB-Regular.otf -r 0x20-0x7E \
  --size 28 --format lvgl --bpp 4 --no-compress \
  -o firmware/src/font_styrene_28.c --lv-include "lvgl.h"

# DejaVu Sans Mono (animation, with spinner Unicode chars)
lv_font_conv --font assets/DejaVuSansMono.ttf \
  -r 0x20-0x7E,0xB7,0x2026,0x2722,0x2733,0x2736,0x273B,0x273D \
  --size 18 --format lvgl --bpp 4 --no-compress \
  -o firmware/src/font_mono_18.c --lv-include "lvgl.h"
```

**Important:** `lv_font_conv` v1.5.3 outputs LVGL 8 format. Each generated file must be patched for LVGL 9 compatibility:

1. Remove `#if LVGL_VERSION_MAJOR >= 8` guards around `font_dsc` and the font struct
2. Remove the `.cache` field from `font_dsc`
3. Add `.release_glyph = NULL`, `.kerning = 0`, `.static_bitmap = 0` to the font struct
4. Add `.fallback = NULL`, `.user_data = NULL` to the font struct

Without these patches, fonts compile but render as invisible.

## Licensing gray area warning

The software in this repository uses and adheres to the Anthropic brand guidelines and uses the same proprietary fonts that Anthropic has a licnese for but this software uses without permission as well as using assets from Anthropic such as the copyrighted Claude Code mascot head logo so even though the code in this repo is non-proprietary I will not license it myself under a copyleft license since this repo includes proprietary fonts and copyrighted assets. Please be aware of this if you fork or copy the code from this repo. **You have been warned!**
