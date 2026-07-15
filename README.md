# AI Usage Monitor

A small ESP32 desk dashboard that tracks usage across **multiple AI coding CLIs at once** —
Claude Code, OpenAI Codex, and Antigravity (Gemini) — plus a live view of your
host machine's CPU / GPU / RAM.

> **Fork notice.** This project is a multi-provider fork of
> [HermannBjorgvin/Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter),
> which tracks Claude Code only. It adds per-provider swipe tabs, a `/stats`
> screen with an activity heatmap, and a host-resource view. Full credit for the
> original firmware, HAL, splash engine, and BLE daemon goes to the upstream
> author — see [`NOTICE.md`](NOTICE.md).

It runs on a [Waveshare ESP32-S3-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm?&aff_id=149786)
(and a few sibling boards) and pairs over Bluetooth. A Linux/macOS/Windows daemon
polls each provider and pushes compact JSON over BLE GATT; the board renders it.
The splash screen plays pixel-art Clawd animations that get busier as your usage
climbs. The two side buttons send Space and Shift+Tab over BLE HID for Claude
Code's voice mode and mode-toggle shortcuts.

The Clawd animations come from [claudepix](https://claudepix.vercel.app), [@amaanbuilds](https://x.com/amaanbuilds)'s library of pixel-art Clawd sprites, check it out, it's lovely.

## Screens & navigation

The device boots into the splash. It has one usage tab per provider plus a host
System tab, laid out left-to-right, and a per-provider Stats screen:

```
              (swipe left  <->  swipe right)
   System  <->  Claude  <->  Codex  <->  Antigravity
                          |
              tap the title on any tab
                          v
                        Stats   (heatmap - tokens - model - sessions - streak)
```

- **Swipe left / right** to page between the System, Claude, Codex, and Antigravity tabs.
- **Tap the title** ("Usage" / "Codex" / ...) to open that provider's **Stats** screen — a GitHub-style activity heatmap, total tokens, favourite model, session count, longest session, current/best streak, and a whimsical "~Nx more tokens than Dune" line. Tap the title again to go back.
- **Tap the logo** (top-left) to jump to the splash; tap the splash to return.

Each usage tab shows the provider's limits as bars: Claude has a 5-hour session
window and a weekly window; Codex has one weekly window plus a context-length
gauge; Antigravity has 5-hour and weekly windows for its Gemini model pool. Any
provider whose CLI isn't installed or logged in is simply omitted — the tabs that
remain still work. A subtitle under each title shows the detected plan (e.g.
"Claude Max 20x", "Codex Plus", "Gemini Models").

|              Splash               |              Usage              |
| :-------------------------------: | :-----------------------------: |
| ![Splash](screenshots/splash.png) | ![Usage](screenshots/usage.png) |
|   Splash; tap the logo anytime    | Per-provider session and weekly bars |

While the splash is up, the middle (PWR) button cycles animations, and the tab
buttons/brightness controls still apply. **Hold the power button for 3 seconds, then release, to put the device into pairing mode** — this clears the saved Bluetooth bond and re-advertises. The firmware also auto-rotates animations every 20 s within the current usage-rate group, so a long stretch on the splash isn't just one Clawd on loop.

## Hardware

Boards supported out of the box:

- [Waveshare ESP32-S3-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm?&aff_id=149786)
- [Waveshare ESP32-C6-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-c6-touch-amoled-2.16.htm?&aff_id=149786) 
- [Waveshare ESP32-S3-Touch-AMOLED-1.8](https://www.waveshare.com/esp32-s3-touch-amoled-1.8.htm?&aff_id=149786)
- [Waveshare ESP32-C6-Touch-AMOLED-1.8](https://www.waveshare.com/esp32-c6-touch-amoled-1.8.htm?&aff_id=149786)

> Please check if a pull request exists for your alternative hardware port before opening a new one, providing QA feedback and testing on the same hardware is more valuable than duplicate pull requests.

**Porting to another board:** the firmware is a thin HAL with per-board folders under `firmware/src/boards/`. Drop in a new folder and a new PlatformIO env — `main.cpp`, `ui.cpp`, and `splash.cpp` never need to change. See [`docs/porting/adding-a-board.md`](docs/porting/adding-a-board.md) for the walk-through and [`docs/porting/hal-contract.md`](docs/porting/hal-contract.md) for the interfaces a port must implement.

## Prerequisites

- Linux (tested on Ubuntu), macOS, or Windows 10/11
- [PlatformIO CLI](https://docs.platformio.org/en/latest/core/installation/index.html)
- Linux: `curl`, `bluetoothctl`, `busctl` (BlueZ Bluetooth stack)
- macOS: `python3` (the installer sets up a venv with `bleak` and `httpx`)
- Windows: `python3` 3.11+ (the installer sets up a venv with `bleak`, `httpx`, and `pystray`)
- Claude Code with an active subscription (required)
- Optional, each adds its own tab if present: **OpenAI Codex** CLI (logged in, `~/.codex/auth.json`), **Antigravity** / Gemini CLI (`agy`, logged in). The System tab needs no extra setup on Linux.

## macOS installation

The macOS host pieces — Python daemon, LaunchAgent, and flash helper — were ported by [Chris Davidson (@lorddavidson)](https://github.com/lorddavidson). Thanks Chris!

### Flash the firmware

```bash
./flash-mac.sh waveshare_amoled_216                       # auto-detects /dev/cu.usbmodem*
./flash-mac.sh waveshare_amoled_18  /dev/cu.usbmodem1101  # or pass an explicit USB serial port
```

The board env name is required. Run `./flash-mac.sh` with no args to see the available envs (scraped from `firmware/platformio.ini`).

### Pair the device

After flashing, open **System Settings → Bluetooth** and click *Connect* next to "Clawdmeter". The daemon only ever connects to the peripheral this Mac is paired/connected to — it never scans for a nearby device — so once it's connected here the daemon picks it up on its next poll (~60 s).

### Install the daemon

The daemon reads your Claude OAuth token from the macOS Keychain (service `Claude Code-credentials`), polls usage every 60 s, and pushes it to the display over BLE.

```bash
./install-mac.sh
```

The installer creates a Python venv in `daemon/.venv/`, installs `bleak` and `httpx`, renders a LaunchAgent into `~/Library/LaunchAgents/com.user.claude-usage-daemon.plist`, and loads it. The first run is launched interactively so macOS prompts for Bluetooth permission.

Useful commands:

```bash
launchctl list | grep claude-usage                                          # check it's running
tail -F ~/Library/Logs/claude-usage-daemon.out.log                          # live logs
launchctl unload ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist  # stop
launchctl load -w ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist # start
```

## Linux installation

### Flash the firmware

```bash
./flash.sh waveshare_amoled_216                  # defaults to /dev/ttyACM0
./flash.sh waveshare_amoled_18  /dev/ttyACM1     # or pass an explicit USB serial port
```

The board env name is required. Run `./flash.sh` with no args to see the available envs (scraped from `firmware/platformio.ini`).

### Pair the device

After flashing, the device advertises as "Clawdmeter". Pair it once:

```bash
# Scan for the device
bluetoothctl scan le

# When "Clawdmeter" appears, pair and trust it
bluetoothctl pair F4:12:FA:C0:8F:E5    # use your device's MAC
bluetoothctl trust F4:12:FA:C0:8F:E5
```

To re-pair later, hold the power button for 3 seconds then release — the device clears its saved bond and re-advertises.

### Install the daemon

The daemon polls your Claude usage every 60 seconds and sends it to the display over BLE.

```bash
./install.sh
systemctl --user start claude-usage-daemon
```

Check status: `systemctl --user status claude-usage-daemon`

View logs: `journalctl --user -u claude-usage-daemon -f`

## Windows installation

Runs natively on Windows — no WSL required. A system-tray app polls your usage and pushes it over BLE, and starts automatically at login.

### Prerequisites

- **Native Windows** (not WSL).
- **Python 3.11+** from [python.org](https://www.python.org/downloads/) — check *"Add python.exe to PATH"* during install.
- **Claude Code** installed, with `claude login` completed. The token is read from `%USERPROFILE%\.claude\.credentials.json` (falling back to `%LOCALAPPDATA%\Claude\` then `%APPDATA%\Claude\`).
- The repo on a **native Windows path** (e.g. `%USERPROFILE%\Clawdmeter`), **not** a `\\wsl$` share — the installer refuses a WSL path.

### Flash the firmware

```powershell
pio run -d firmware -e waveshare_amoled_216 -t upload --upload-port COM5   # use your device's COM port
```

Run `pio run -d firmware` with no env to see the available board envs.

### Pair the device

The device is a bonded BLE HID keyboard, so pair it once: **Settings → Bluetooth & devices → Add device → Bluetooth**, then select "Clawdmeter". Pairing is **required** — it enables the physical buttons and keeps a persistent connection (the device keeps showing your last-synced usage even after the daemon quits). To undo, use **Remove device** (this disables the buttons).

### Install the daemon (recommended)

From the repo root in PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File install-windows.ps1
```

This creates a venv, installs `bleak`/`httpx`/`pystray`/`Pillow` from the in-repo requirements (no internet downloads), registers a per-user login-autostart entry (`HKCU\…\Run`, no admin needed), and launches the tray app headlessly (no console window).

### Run manually instead (optional)

```powershell
python -m venv .venv
.venv\Scripts\Activate.ps1        # if blocked: Set-ExecutionPolicy -Scope CurrentUser RemoteSigned, then retry
pip install -r daemon\requirements-windows.txt
python daemon\claude_usage_daemon_windows.py        # runs in the foreground; Ctrl+C to stop
```

### Tray icon and menu

The icon's corner bubble shows state — **green** Connected, **amber** Scanning, **red** Error — and hovering shows the status (`Connected · last update HH:MM`). A notification fires once when it enters Error (e.g. an expired token). Right-click for the menu:

- **Status header** — live state + last sync time.
- **Start at login** — toggle autostart on/off.
- **Quit** — stops the daemon cleanly; leaves the Windows pairing intact (device keeps its last reading).

### Logs and troubleshooting

```powershell
Get-Content $env:LOCALAPPDATA\Clawdmeter\daemon.log -Tail 30        # view logs
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v Clawdmeter /f   # remove autostart
```

| Symptom | Fix |
|---------|-----|
| `Device not found` | Power on the device; make sure it's in range and paired. |
| `token expired` toast / `API HTTP 401` | Re-run `claude login`, then restart the daemon. |
| `Connection failed` | Toggle Windows Bluetooth off/on in Settings. |
| `Warning: running under Linux/WSL` | Run from a native PowerShell window, not a WSL shell. |

## How it works

The daemon polls every provider it can find each cycle (~30 s), merges the
results into one payload, and pushes it to the board over BLE. Each provider is
independent — a missing or logged-out CLI just drops its keys from the payload.

- **Claude** — reads the OAuth token (macOS Keychain `Claude Code-credentials`, else `~/.claude/.credentials.json` / `%USERPROFILE%\.claude\.credentials.json`), makes a 1-token Haiku call to `api.anthropic.com/v1/messages`, and reads the usage numbers from the `anthropic-ratelimit-unified-*` **response headers** (the body is thrown away). The 5-hour and weekly windows come from these headers.
- **Codex** — reads the ChatGPT OAuth token from `~/.codex/auth.json` and GETs `chatgpt.com/backend-api/wham/usage` (the endpoint the Codex CLI itself polls) for the weekly window; the live context-length gauge comes from the newest `~/.codex/sessions/**/rollout-*.jsonl`.
- **Antigravity (Gemini)** — the `agy` CLI runs a local language server; the daemon finds its port from `~/.gemini/antigravity-cli/log/` and POSTs to `127.0.0.1:<port>/…/RetrieveUserQuotaSummary`. The local server does the auth, so the daemon needs no token of its own. The last good response is cached so the tab keeps showing while `agy` is closed. Only the "Gemini Models" pool (5-hour + weekly) is shown.
- **System** — CPU / GPU / RAM utilization and temperatures from the host, refreshed each cycle.
- **Stats** — Claude's come from `~/.claude/stats-cache.json` (the same file `/stats` reads) with today's activity merged live from the session transcripts; Codex's and Antigravity's are aggregated from their session logs.

The daemon connects to the ESP32 over BLE and writes a JSON payload to the GATT
RX characteristic; the firmware parses it and updates the LVGL dashboard. It also
tracks the rate of change of Claude's session % over a 5-minute window and picks
splash animations from the matching mood group. The two side buttons are
independent of all this — they send Space and Shift+Tab as BLE HID keyboard input
to the paired host directly.

## Controls

**Touch** drives navigation: swipe left/right to page between the System, Claude,
Codex, and Antigravity tabs; tap a title to open/close that provider's Stats
screen; tap the logo to toggle the splash.

**Buttons** — the board has three side buttons. Left and right send HID keys; the
middle (PWR) button cycles splash animations and, held for 3 seconds, triggers
pairing mode.

| Button           | GPIO         | Function                                                       |
| ---------------- | ------------ | -------------------------------------------------------------- |
| **Left**         | GPIO 0       | Hold to send Space (Claude Code voice-mode push-to-talk)       |
| **Middle** (PWR) | AXP2101 PKEY | On splash: cycle animations. On a tab: cycle brightness. Hold 3s + release: pairing mode |
| **Right**        | GPIO 18      | Press to send Shift+Tab (Claude Code mode toggle)              |

Space and Shift+Tab go out as standard BLE HID keyboard reports, so they trigger in whatever window has focus on the paired host — not just Claude Code.

## BLE protocol

The device advertises a custom GATT service alongside the standard HID keyboard service:

|                            | UUID                                   |
| -------------------------- | -------------------------------------- |
| **Data Service**           | `4c41555a-4465-7669-6365-000000000001` |
| RX Characteristic (write)  | `4c41555a-4465-7669-6365-000000000002` |
| TX Characteristic (notify) | `4c41555a-4465-7669-6365-000000000003` |
| **HID Service**            | `00001812-0000-1000-8000-00805f9b34fb` |

Two payload shapes are written to RX. Every key is optional — a provider whose
CLI is absent or logged out just omits its keys, and the firmware falls back
gracefully. The **usage** payload carries all live bars:

```json
{
  "s": 45, "sr": 120, "w": 28, "wr": 7200, "st": "allowed", "pl": "Claude Max 20x",
  "cx": 92, "cxr": 9932, "cxw": 10080, "cxpl": "Codex Plus", "ctx": 113580, "ctxw": 258400,
  "ag5": 44, "ag5r": 136, "agw": 7, "agwr": 9916, "agpl": "Gemini Models",
  "cpu": 12, "ct": 66, "gpu": 3, "gt": 37, "ram": 65,
  "ok": true
}
```

| Prefix | Provider | Keys |
| ------ | -------- | ---- |
| `s`/`w` | Claude | `s`/`w` = 5h/weekly %, `sr`/`wr` = reset mins, `st` = status, `pl` = plan |
| `cx` | Codex | `cx` = used %, `cxr` = reset mins, `cxw` = window mins, `cxpl` = plan, `ctx`/`ctxw` = context tokens / window |
| `ag` | Antigravity | `ag5`/`agw` = 5h/weekly %, `ag5r`/`agwr` = reset mins, `agpl` = plan |
| `cpu`/`gpu`/`ram` | System | percentages; `ct`/`gt` = CPU/GPU temp °C |

The **stats** payload is marked with `sv` and carries one provider's `/stats`
figures (`p` = provider, `tt` = total tokens, `fm` = favourite model, `ns` =
sessions, `cs`/`bs` = current/best streak, `hm` = 49-char heatmap, …). It is sent
separately per provider because all of it plus the usage bars would overflow the
512-byte RX buffer.

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

# Styrene B (numbers 48, labels 28, small text 24/20, dense stat rows 16/14/12)
for size in 48 28 24 20 16 14 12; do
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

The UI uses a small set of [Lucide](https://lucide.dev) icons (bluetooth + battery states) converted to RGB565 / RGB565A8 C arrays for LVGL.

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

- Original **Clawdmeter** project by [Hermann Björgvin](https://github.com/HermannBjorgvin/Clawdmeter) — firmware, board HAL, splash engine, and BLE daemon. This repo is a multi-provider fork; see [`NOTICE.md`](NOTICE.md).
- Pixel-art Clawd animation by [@amaanbuilds](https://x.com/amaanbuilds), sourced from [claudepix.vercel.app](https://claudepix.vercel.app). Frame data and palettes scraped + converted by the tooling in `tools/`.
- Lucide icon set ([lucide.dev](https://lucide.dev), MIT) for bluetooth and battery UI glyphs.
- Anthropic brand fonts (Tiempos Text, Styrene B) and the Clawd mascot; **OpenAI** and **Google Gemini** logos as provider marks — all property of their owners, included for personal use only. See the licensing note below and [`NOTICE.md`](NOTICE.md).

## Licensing gray area warning

The software in this repository uses and adheres to the Anthropic brand guidelines and uses the same proprietary fonts that Anthropic has a license for but this software uses without permission as well as using assets from Anthropic such as the copyrighted Clawd mascot so even though the code in this repo is non-proprietary I will not license it myself under a copyleft license since this repo includes proprietary fonts and copyrighted assets. Please be aware of this if you fork or copy the code from this repo. **You have been warned!**
