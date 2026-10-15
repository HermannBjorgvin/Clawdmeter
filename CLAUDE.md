# Clawdmeter

A desk-side Claude Code usage monitor. ESP32-S3 / ESP32-C6 firmware
(`firmware/`) shows usage meters and Clawd, Anthropic's pixel mascot; a Rust
desktop app (`desktop/`) reads usage, follows Claude Code hook events and
writes them to the device over BLE.

## Repo map

- `firmware/`: PlatformIO firmware. Read `firmware/CLAUDE.md`; each board's
  pins and hardware gotchas are in `firmware/src/boards/<env>/CLAUDE.md`.
- `desktop/`: Rust workspace (tray, window, daemon, hooks, flasher). Read
  `desktop/CLAUDE.md`.
- `docs/`: `protocol.md` (BLE payload), `clawd-director.md` (everything Clawd
  does), `porting/`, `fonts.md`, `development.md`, `user-guide.md`,
  `daemon-parity.md` (the removed bash/Python daemons → the app).
- `tools/`: asset converters, `ci/` (release and manifest scripts, with
  unittests), `packaging/`. `research/clawd-official/`: the official Clawd
  animation archive.
- `SIM-USAGE.md`: simulator manual; `docs/sim-grid.md`: every panel at once
  (`tools/simgrid/`). `screenshot.sh`: device screenshot over serial.

Nested `CLAUDE.md` files load when you read or edit a file in their folder.

## Boards

| Env | Board | Notes |
| --- | --- | --- |
| `waveshare_amoled_216` | Waveshare ESP32-S3-Touch-AMOLED-2.16 | CO5300 480×480, CST9220 touch, IMU rotation |
| `waveshare_amoled_18` | Waveshare ESP32-S3-Touch-AMOLED-1.8 | 368×448; two panel revisions, detected at boot |
| `waveshare_amoled_216_c6` | Waveshare ESP32-C6-Touch-AMOLED-2.16 | C6 (no PSRAM), CO5300 480×480 |
| `waveshare_amoled_18_c6` | Waveshare ESP32-C6-Touch-AMOLED-1.8 | C6 (no PSRAM), SH8601 368×448 |
| `waveshare_amoled_206` | Waveshare ESP32-S3-Touch-AMOLED-2.06 | CO5300 410×502 watch, 32 MB flash |
| `waveshare_lcd_154` | Waveshare ESP32-S3-Touch-LCD-1.54 | ST7789 SPI TFT 240×240, no PMU |
| `waveshare_lcd_4` | Waveshare ESP32-S3-Touch-LCD-4 | ST7701 RGB 480×480, one button |
| `lilygo_t4s3` | LILYGO T4-S3 | RM690B0 AMOLED 450×600, CST226SE touch, SY6970 charger |
| `waveshare_lcd_183` | Waveshare ESP32-S3-Touch-LCD-1.83 | ST7789P SPI 240×284, CST816 touch, AXP2101 |
| `esp32_s3_4848s040` | Guition-style ESP32-S3 4848S040 | ST7701S RGB 480×480, GT911 touch, no buttons readable |
| `waveshare_amoled_143` | Waveshare ESP32-S3-Touch-AMOLED-1.43 | CO5300/SH8601 466×466 round, two revisions detected at boot, no PWR button |
| `waveshare_amoled_132` | Waveshare ESP32-S3-Touch-AMOLED-1.32 | CO5300 466×466 round, CST820 touch, BOOT rotates the UI |
| `esp32_2432s024c`, `esp32_2432s024c_landscape` | Sunton ESP32-2432S024C | classic ESP32, no PSRAM, ST7789 SPI 240×320 / 320×240, CST820 touch |
| `esp32_2432s028r`, `esp32_2432s028r_landscape` | Sunton ESP32-2432S028R (CYD) | classic ESP32, no PSRAM, ILI9341 SPI 240×320 / 320×240, resistive XPT2046 touch |
| `m5sticks3` | M5Stack M5StickS3 | ST7789P3 SPI 135×240, no touch, M5PM1 PMIC, ES8311 chime |
| `m5stack_fire` | M5Stack FIRE | ESP32 (classic, no S3), ILI9342C SPI 320×240, no touch, 3 buttons, IP5306 |
| `lilygo_t_display_s3` | LILYGO T-Display S3 (non-touch) | ST7789 i80 parallel 170×320, BOOT + a PWR-role key |
| `waveshare_lcd_2` | Waveshare ESP32-S3-Touch-LCD-2 | ST7789T3 SPI 240×320, CST816D touch, no usable buttons |
| `waveshare_epaper_154` | Waveshare ESP32-S3-ePaper-1.54 | SSD1681 e-paper 200×200, no touch, `minimal_ui` |
| `xingzhi_cube_183` | Xingzhi Cube 1.83 TFT WiFi | ST7789-type SPI 284×240 landscape, no touch, 3 buttons, battery ADC |
| `freenove_s3_display_nontouch` | Freenove ESP32-S3 Display (FNK0104A, non-touch) | ILI9341 SPI 240×320, minimal UI, BOOT is the only button |
| `sensecap_indicator_d1l` | Seeed SenseCap Indicator D1L | ST7701S RGB 480×480, FT6x36 touch, PCA9535; GPIO 38 key is the PWR role |
| `spotpear_s3_154` | SpotPear ESP32-S3-LCD-1.54 MUMA | ST7789 SPI 240×240, CST816 touch, no PMU, BOOT only |
| `sim` | SDL2 desktop simulator | `platform = native`, 480×480 by default |
| `native` | Host unit tests | |

A new port needs a board folder, an env, a board `CLAUDE.md`, original SVG
art, and app/README/manifest entries: use the `porting-a-board` skill.

## Build and test

```bash
pio run -d firmware                                  # default_envs: every hardware env + sim
pio run -d firmware -e waveshare_amoled_216          # one board
pio test -d firmware -e native                       # host tests; always pass -e native
pio run -d firmware -e sim && (cd firmware && .pio/build/sim/program)   # simulator (SDL2 dev files)
python3 tools/simgrid/simgrid.py serve               # every panel at once (docs/sim-grid.md)
(cd desktop && cargo test --workspace --locked)      # fmt/clippy/Windows check: desktop/CLAUDE.md
python3 -m unittest discover -s tools/ci/tests        # release/manifest scripts
```

A bare `pio test` runs every default env, hardware included. If `pio` isn't on
PATH, use `~/.platformio/penv/bin/pio` (or `brew install platformio`). On
Linux, `nix develop` gives Rust, PlatformIO and SDL2. Flashing and serial
commands: `flashing-and-probing-boards` skill.

## Device protocol

The app writes one complete JSON object per write to RX
(`4c41555a-…-0002`); the firmware buffer is 512 B and the app caps writes at
500. Every write is a full update. Fields and UUIDs: `docs/protocol.md`.

- Usage: `s sr w wr st acct tp pd rd t tf c ok`. `sr`/`wr` = -1 means not
  started or unknown. `acct:"ent"` is a spend-capped account: `s` is spend %,
  `tp/pd/rd` the billing period. `{"ok":false}` means no data.
- Attention: `b` (alert chime), `m` (ASCII message), `x` (clear).
- Fast clock: `a` (activity 0–3), `e` + `es` (event `p`/`s`/`b` and its
  sequence), `n` (active sessions). `rp:1` marks a heartbeat replay, which the
  rate math skips.
- Clawd's settings, on every data payload: `lv` (liveliness 1–5) and `aa`
  (attention animation seconds: 120, 30, 10, 0 = off; only his animation,
  not the alert). The device keeps both in NVS (`clawd_prefs.cpp`) so they
  apply from boot.
- Device → app: `0x01` on REQ (`…-0004`) when it has no data yet;
  `{"dismiss":true}` on TX (`…-0003`) when a tap or the 2-minute timeout ends
  an alert. The app clears its own alert and doesn't echo a clear back. Alerts
  nobody clears expire in the app after 2 minutes (a test alert after 30 s);
  if the device still shows one, the app sends `x`.

Adding or renaming a key means changing both sides: `clawdmeter-core`'s
`keys_match_firmware_parser` test cross-checks the app's keys against
`doc["…"]` in `firmware/src/*.cpp`.

## Hard rules

- **No `#ifdef BOARD_*` in shared firmware code.** Use `BoardCaps`, the HAL
  or a per-board file.
- **One Clawd on screen, ever** (`firmware/CLAUDE.md` gotcha 2).
- **No data is never a zero.** The firmware feeds only `ok && !rp` payloads to
  the rate math and reset detector; in the app a failed poll is never a 0 %
  reading.
- **The usage probe must never start a 5-hour window** (owner's rule; how
  `choose_source` enforces it is in `desktop/CLAUDE.md`).
- **Tests never run a real `systemctl`, `launchctl` or `reg`** on the
  developer's machine.
- **The chime is described only by its note and envelope** (C#5, 554.5 Hz,
  attack/strike/ring) in code, docs, commits and PRs. Never name or hint at
  what inspired it, and add no credit line for it.
- **Clawd sprite art comes only from the official animations** in
  `research/clawd-official/`; never draw or retouch sprite frames. The large
  screensaver face is the exception: it's built from LVGL shapes and may be
  embellished. Board illustrations are original SVGs, never copied photos.
- **Check UI changes yourself** before handing over: the
  `verifying-firmware-ui` and `capturing-desktop-screenshots` skills.
  Simulator captures don't count as hardware testing.
- Lint `.github/` changes with `actionlint` (and `shellcheck`) before pushing.

## Pull requests

Use `.github/pull_request_template.md` and write for a reviewer seeing the
change for the first time.

- Describe the end state: behaviour, structure and file paths. Leave out the
  session history, abandoned approaches and checks CI already gates. Keep it
  proportional to the change.
- Add screenshots or photos when UI or device output changes (device output:
  `./screenshot.sh` captures or photos). Label simulator captures as such.
  Add a Mermaid diagram when it clarifies flow, state or architecture.
- **Hardware testing**: the exact boards and revisions tested on real
  hardware and what was verified, or "not tested on hardware".
- **Human involvement**: how much the human author reviewed and QA'd, e.g.
  "Agent-written; human reviewed the diff and tested pairing and touch on an
  ESP32-S3-Touch-AMOLED-2.16." If none, "not reviewed by a human". Report
  only confirmed involvement; approval to proceed is not review.
- A new board also needs its buy link and preferably a photo of it running
  Clawdmeter (say if there is none). A marketplace link (Amazon, AliExpress)
  is fine when the maker has no store of its own. Its SVG and catalog entries
  are part of the port, not follow-up work.

## CI

Workflows in `.github/workflows/`:

- `firmware.yml`: builds every hardware env plus merged images, runs native
  tests, captures a headless sim screenshot, runs `tools/ci` unittests.
- `desktop.yml`: fmt, clippy and workspace tests on Linux; core tests on
  macOS and Windows.
- `release.yml`: runs on `v*` tags; see the `cutting-a-release` skill.

## Skills

Project skills in `.claude/skills/`:

- `porting-a-board`: add a board end to end, including app, README, manifest and PR requirements.
- `verifying-firmware-ui`: simulator screenshots and on-device `clawd <beat>` + `screenshot` QA.
- `flashing-and-probing-boards`: flash a board and run serial commands (bundled `serial_cmd.py`).
- `capturing-desktop-screenshots`: demo window screenshots under X11 (bundled `capture.sh`).
- `cutting-a-release`: tags, what the release builds, pre-tag and packaging checks.
- `regenerating-firmware-assets`: fonts, RGB565A8 icons, the Clawd animation header.
- `hunting-clawd-animations`: find and archive new official Clawd animations.
