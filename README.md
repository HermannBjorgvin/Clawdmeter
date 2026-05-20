# Clawdmeter

A small ESP32 dashboard I made for my desk to keep an eye on Claude Code usage.

It runs on a [Waveshare ESP32-S3-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm?&aff_id=149786) and talks to the host daemon over **USB CDC serial** (this is the `usb-transport` branch; `main` uses BLE GATT instead). The splash screen plays pixel-art Clawd animations that get busier when your usage rate climbs.

> **Why USB?** The BLE pairing dance on Linux/bluez can be flaky — half-bonded links, stale GATT handles after firmware reboots, alternating write failures from missing ACKs. USB CDC is a wire; if you're plugging the board in for power anyway, the data link rides for free with zero pairing state.

|              Usage meter              |              Clawd animation screen              |
| :-----------------------------------: | :----------------------------------------------: |
| ![Usage meter](assets/demo.jpeg) | ![Clawd animation screen](assets/demo.gif) |

The Clawd animations come from [claudepix](https://claudepix.vercel.app), [@amaanbuilds](https://x.com/amaanbuilds)'s library of pixel-art Clawd sprites, check it out, it's lovely.

## Screens

The device boots into the splash and stays there until you press the middle (PWR) button, which cycles between Usage and Link. Tap the screen anywhere to flip back to the splash; tap again to dismiss it.

|              Splash               |              Usage              |             USB Link             |
| :-------------------------------: | :-----------------------------: | :------------------------------: |
| ![Splash](screenshots/splash.png) | ![Usage](screenshots/usage.png) | Connection + port status         |
|   Splash; touch-toggle anytime    | Session and weekly utilization  | "Connected" / "Waiting" / "Stale" |

While the splash is up, the middle button cycles animations instead of screens. The firmware also auto-rotates every 20 s within the current usage-rate group, so a long stretch on the splash isn't just one Clawd on loop.

## Hardware

- [Waveshare ESP32-S3-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm?&aff_id=149786) - ESP32-S3R8, 2.16" 480×480 AMOLED (CO5300 QSPI), CST9220 cap touch, AXP2101 PMU + Li-Po battery, QMI8658 IMU
- USB-C cable for flashing firmware and charging
- 3.7V Li-Po battery (MX1.25 2-pin connector, optional)

## Prerequisites

- Linux (tested on Arch / CachyOS) or macOS
- [PlatformIO CLI](https://docs.platformio.org/en/latest/core/installation/index.html)
- Linux: `curl`, `awk`, `stty` (all standard); user in the `dialout` (or `uucp`) group for `/dev/ttyACM0` access
- macOS: `python3` (the installer sets up a venv with `pyserial` and `httpx`)
- Claude Code with an active subscription

## macOS installation

### Flash the firmware

```bash
./flash-mac.sh                       # auto-detects /dev/cu.usbmodem*
./flash-mac.sh /dev/cu.usbmodem1101  # or pass an explicit USB serial port
```

### Install the daemon

The macOS daemon reads your Claude OAuth token from the Keychain (service `Claude Code-credentials`), polls usage every 60 s, and writes JSON lines to `/dev/cu.usbmodem*`. No Bluetooth permissions needed.

```bash
./install-mac.sh
```

The installer creates a Python venv in `daemon/.venv/`, installs `pyserial` and `httpx`, renders a LaunchAgent into `~/Library/LaunchAgents/com.user.claude-usage-daemon.plist`, and loads it.

Useful commands:

```bash
launchctl list | grep claude-usage                                          # check it's running
tail -F ~/Library/Logs/claude-usage-daemon.out.log                          # live logs
launchctl unload ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist  # stop
launchctl load -w ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist # start
```

> **Why `/dev/cu.usbmodem*` not `/dev/tty.usbmodem*`?** The `cu.` (callout) device doesn't assert DTR on open. The `tty.` (dial-in) device does, which the ESP32-S3's USB CDC implementation interprets as a reset — the firmware would reboot every time we wrote a payload. The daemon's `find_port()` only matches `cu.*`.

## Linux installation

### Flash the firmware

```bash
cd firmware
pio run -t upload --upload-port /dev/ttyACM0
```

### Install the daemon

The daemon polls your Claude usage every 60 seconds and writes JSON lines to `/dev/ttyACM0`. No pairing — plug in the USB cable and start the service.

```bash
./install.sh
systemctl --user start claude-usage-daemon
```

If you get a permission error on `/dev/ttyACM0`, add yourself to the `dialout` group (the group name may be `uucp` on Arch) and log in again:

```bash
sudo usermod -aG dialout $USER
```

Override the serial port via the `DEVICE_PORT` environment variable (e.g. `DEVICE_PORT=/dev/ttyACM1`) — `systemctl --user edit claude-usage-daemon`.

Check status: `systemctl --user status claude-usage-daemon`

View logs: `journalctl --user -u claude-usage-daemon -f`

## How it works

1. The daemon reads your Claude Code OAuth token from `~/.claude/.credentials.json`.
2. It makes a minimal API call to `api.anthropic.com/v1/messages` — one token of Haiku, basically free.
3. The usage numbers come straight out of the response headers (`anthropic-ratelimit-unified-5h-utilization` and friends).
4. The daemon writes a JSON line to `/dev/ttyACM0`. The firmware's serial line reader (`serial_link.cpp`) parses it and updates the LVGL dashboard.
5. The firmware also tracks the rate of change of session % over a 5-minute window and picks splash animations from the matching mood group.
6. The two side buttons currently don't do anything — they used to send Space and Shift+Tab over BLE HID on `main`. A USB HID composite port is a future TODO so they can do the same over USB.

## Physical buttons

The board has three side buttons. The middle button cycles screens; left and right are reserved for a future USB HID composite port.

| Button           | GPIO         | Function                                                  |
| ---------------- | ------------ | --------------------------------------------------------- |
| **Left**         | GPIO 0       | Reserved (was BLE HID Space on `main`)                    |
| **Middle** (PWR) | AXP2101 PKEY | Cycle screens (Usage ↔ Link); on splash, cycle animations |
| **Right**        | GPIO 18      | Reserved (was BLE HID Shift+Tab on `main`)                |

## Wire protocol

The host daemon and firmware speak a line-delimited (`\n`-terminated) text protocol over `/dev/ttyACM0`:

| Direction      | Line                                                       | Meaning                              |
| -------------- | ---------------------------------------------------------- | ------------------------------------ |
| host → device  | `{"s":45,"sr":120,"w":28,"wr":7200,"st":"allowed","ok":true}` | Usage payload                        |
| host → device  | `screenshot`                                               | Dump current LVGL framebuffer        |
| device → host  | `ACK`                                                      | Last payload accepted                |
| device → host  | `NACK`                                                     | Last payload failed JSON parse       |
| device → host  | `REQ`                                                      | Asking host for a fresh poll        |
| device → host  | `READY`                                                    | Sent once on boot                    |

Payload fields: `s` = session %, `sr` = session reset (minutes), `w` = weekly %, `wr` = weekly reset (minutes), `st` = status, `ok` = success flag.

## USB CDC gotcha

The daemon configures the TTY with `stty -hupcl` — this is **critical**. Without it, every time a process opens `/dev/ttyACM0` the kernel toggles DTR, which the ESP32-S3's USB CDC implementation interprets as a reset request. The firmware would reboot on every write. `-hupcl` keeps DTR steady through open/close cycles.

## Recompiling fonts

The `firmware/src/font_*.c` files are pre-compiled LVGL bitmap fonts.

```bash
npm install -g lv_font_conv
```

Generate each one (one at a time — `lv_font_conv` doesn't like loop-driven invocations) with `--no-compress` (required for LVGL 9):

```bash
# Tiempos Text (titles, 56px)
lv_font_conv --font assets/TiemposText-400-Regular.otf -r 0x20-0x7E \
  --size 56 --format lvgl --bpp 4 --no-compress \
  -o firmware/src/font_tiempos_56.c --lv-include "lvgl.h"

# Styrene B (large numbers 48, panel labels 28, small text 24, minimal 20)
for size in 48 28 24 20; do
  lv_font_conv --font assets/StyreneB-Regular.otf -r 0x20-0x7E \
    --size $size --format lvgl --bpp 4 --no-compress \
    -o firmware/src/font_styrene_${size}.c --lv-include "lvgl.h"
done

# DejaVu Sans Mono (32px, with spinner Unicode chars)
lv_font_conv --font assets/DejaVuSansMono.ttf \
  -r 0x20-0x7E,0xB7,0x2026,0x2722,0x2733,0x2736,0x273B,0x273D \
  --size 32 --format lvgl --bpp 4 --no-compress \
  -o firmware/src/font_mono_32.c --lv-include "lvgl.h"
```

**Important:** `lv_font_conv` v1.5.3 outputs LVGL 8 format. Each generated file must be patched for LVGL 9 compatibility:

1. Remove `#if LVGL_VERSION_MAJOR >= 8` guards around `font_dsc` and the font struct
2. Remove the `.cache` field from `font_dsc`
3. Add `.release_glyph = NULL`, `.kerning = 0`, `.static_bitmap = 0` to the font struct
4. Add `.fallback = NULL`, `.user_data = NULL` to the font struct

Without these patches, fonts compile but render as invisible.

## Converting Lucide icons

The UI uses a small set of [Lucide](https://lucide.dev) icons (battery states) converted to RGB565 / RGB565A8 C arrays for LVGL.

```bash
node tools/png_to_lvgl.js assets/icon_bluetooth_48.png icon_bluetooth_data ICON_BLUETOOTH_WIDTH ICON_BLUETOOTH_HEIGHT
```

Default tint is white (`0xFFFFFF`); Lucide PNGs ship as black-on-transparent and would render invisible against the dark UI without it. Pass `--no-tint` for pre-coloured artwork like the logo. Battery icons use RGB565A8 (alpha plane) so they blend cleanly over the splash; the rest are baked RGB565 over the panel colour. Paste the converter output into `firmware/src/icons.h`.

## Splash animations

The animations come from [claudepix.vercel.app](https://claudepix.vercel.app),
a library of Clawd sprites. `tools/scrape_claudepix.js` evaluates the
site's JavaScript in a Node VM to pull out frame data and palettes, then
`tools/convert_to_c.js` turns everything into RGB565 C arrays and writes
`firmware/src/splash_animations.h`.

To re-pull (e.g. when the source library updates):

```bash
node tools/scrape_claudepix.js
node tools/convert_to_c.js
pio run -d firmware -t upload
```

See `tools/README.md` for details.

## Credits

- Pixel-art Clawd animation by [@amaanbuilds](https://x.com/amaanbuilds), sourced from [claudepix.vercel.app](https://claudepix.vercel.app). Frame data and palettes scraped + converted by the tooling in `tools/`.
- Lucide icon set ([lucide.dev](https://lucide.dev), MIT) for bluetooth and battery UI glyphs.
- Anthropic brand fonts (Tiempos Text, Styrene B) — see licensing warning below.

## Licensing gray area warning

The software in this repository uses and adheres to the Anthropic brand guidelines and uses the same proprietary fonts that Anthropic has a license for but this software uses without permission as well as using assets from Anthropic such as the copyrighted Clawd mascot so even though the code in this repo is non-proprietary I will not license it myself under a copyleft license since this repo includes proprietary fonts and copyrighted assets. Please be aware of this if you fork or copy the code from this repo. **You have been warned!**
