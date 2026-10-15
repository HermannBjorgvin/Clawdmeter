# Project context

ESP32-S3 / ESP32-C6 firmware for a desk-side Claude Code usage monitor, plus
the Rust desktop app (`desktop/`) that feeds it over BLE. Each
supported board lives in its own `firmware/src/boards/<name>/` folder and is
selected via PlatformIO's `build_src_filter`. A new port needs a board folder,
a PlatformIO environment, original SVG board art, and app/README catalog
entries with a display name and buy link. Shared code (`main.cpp`, `ui.cpp`,
the director modules) stays board-independent. See
[`docs/porting/adding-a-board.md`](docs/porting/adding-a-board.md).

Seven ports today (two SoC families, five panel sizes):

- `boards/waveshare_amoled_216/` — original Waveshare ESP32-S3-Touch-AMOLED-2.16 (CO5300, 480×480 square, CST9220 touch, IMU rotation). Build env: `waveshare_amoled_216`.
- `boards/waveshare_amoled_18/` — Waveshare ESP32-S3-Touch-AMOLED-1.8 (368×448 portrait, XCA9554 IO expander). Build env: `waveshare_amoled_18`. **Two panel revisions are auto-detected at boot** (`board_rev()` in `board_init.cpp`, enum in `board_rev.h`): original = SH8601 display + FT3168 touch (0x38); later = CO5300 display + CST816 touch (0x15). One binary drives both.
- `boards/waveshare_amoled_216_c6/` — Waveshare ESP32-C6-Touch-AMOLED-2.16 (SH8601, 480×480, CST9217 touch). Build env: `waveshare_amoled_216_c6`. ESP32-C6 SoC: single-core RISC-V, **no PSRAM**, BLE 5 only.
- `boards/waveshare_amoled_18_c6/` — Waveshare ESP32-C6-Touch-AMOLED-1.8 (368×448 portrait, SH8601, FT3168 touch, TCA9554 expander). Build env: `waveshare_amoled_18_c6`. Same panel as the S3 1.8 but on the C6 SoC. All subsystems (display, touch, BOOT + PWR buttons, battery, BLE) verified on hardware.
- `boards/waveshare_amoled_206/` — Waveshare ESP32-S3-Touch-AMOLED-2.06 (CO5300, 410×502 watch form factor, FT3168 touch, no IO expander, 32 MB flash, PCF85063 RTC, ES8311 codec). Build env: `waveshare_amoled_206`. Display, touch, battery, IMU init, and BLE verified on hardware; the ES8311 chime path is not wired up (`sound.cpp` no-ops).
- `boards/waveshare_lcd_154/` — Waveshare ESP32-S3-Touch-LCD-1.54 (ST7789, 240×240 square, CST816T touch @ 0x15). Build env: `waveshare_lcd_154`. **The first non-AMOLED port**: a plain 4-wire SPI TFT, not QSPI, and the panel has no brightness command — backlight is LEDC PWM on `LCD_BL`. **No PMU**: battery is an ADC divider on GPIO1 and `BAT_EN` (GPIO2) is a power-hold line that must be driven HIGH early in `board_init()` or the board browns out on battery. Three buttons (BOOT + GPIO5 + a PWR-role GPIO4); ES8311 chime wired up; QMI8658 populated but unused (fixed orientation, no rotation).
- `boards/waveshare_lcd_4/` — Waveshare ESP32-S3-Touch-LCD-4 (ST7701 RGB parallel, 480×480 square, GT911 touch). Build env: `waveshare_lcd_4`. **RGB-panel port**: Arduino_ESP32RGBPanel + bounce buffers (tearing fix). IO expander @ 0x24 (TCA9554 / CH32V003) must init before `gfx->begin()` or the panel stays dark; backlight is expander pin 2 (on/off only). No AXP2101 / IMU; KEY/PWR is hardware RST. Single BOOT button (GPIO 0 → Space/PTT).

Plus one non-hardware target: `boards/sim/` — **native desktop simulator** (SDL2 window, 480×480, `platform = native`). Build env: `sim`. See "Desktop simulator" below.

**C6 ports have no PSRAM** — shared code gates on `BOARD_HAS_PSRAM` (absent on C6) to use `MALLOC_CAP_INTERNAL` for LVGL/splash buffers, and the `screenshot` serial command is disabled (`LV_USE_SNAPSHOT=0`), so UI changes on a C6 board must be eyeballed on hardware, not auto-captured.

The shared code calls a small HAL (`firmware/src/hal/`) that each board implements: display, touch, input, power, IMU, sound, board info. Optional features are guarded by `BoardCaps` (runtime) and `BOARD_HAS_*` (compile-time) rather than `#ifdef BOARD_*`.

The desktop app reads usage with `claude -p /usage`, follows Claude Code hook events, and writes one JSON payload per change to the device over BLE. The old bash/Python daemons and install scripts are gone (see `docs/daemon-parity.md`). This file is for future Claude Code sessions to bootstrap quickly. Read this first.

## Hardware (critical pins)

### AMOLED-2.16 (original)
- Display: **CO5300** AMOLED via QSPI (CS=12, SCLK=38, SDIO0..3=4..7, RST=2)
- Touch: **CST9220** via I2C (SDA=15, SCL=14, INT=11, addr=0x5A)
- PMU: **AXP2101** on same I2C bus (addr=0x34) — battery, USB VBUS, PWR button IRQ
- IMU: **QMI8658** on same I2C bus (addr=0x6B) — accelerometer for auto-rotation
- Buttons: GPIO 0 (left → Space/voice-mode), GPIO 18 (right → Shift+Tab/mode-toggle), AXP PKEY (middle → PWR: brightness; on the stage → next scene; hold → pair)

### AMOLED-1.8 (newer port)
**Two hardware revisions ship under this name; the firmware probes I2C at boot and picks drivers automatically (`board_rev()`):**
- Display: **SH8601** (original) or **CO5300** (later rev) AMOLED via QSPI (CS=12, **SCLK=11** ← different!, SDIO0..3=4..7, RST routed via XCA9554 EXIO1). Both are `Arduino_OLED` subclasses held behind one base pointer in `display.cpp`. The CO5300's 368-wide active area starts at GRAM column 16, so it gets `CO5300_COL_OFFSET 16` to center; SH8601 needs none.
- Touch: **FT3168** @ 0x38 (original) or **CST816** @ 0x15 (later rev), via I2C (SDA=15, SCL=14, INT=21). Both expose the same FocalTech-style data layout at regs 0x02..0x06, so one inline reader in `touch.cpp` serves both — only the address differs. Avoids vendoring the GPLv3 `Arduino_DriveBus` library. Revision is detected by which touch address ACKs (CST816 present ⇒ CO5300 panel).
- PMU: AXP2101 @ 0x34 (same chip as 2.16 — `XPowersLib` reused; battery is an optional kit add-on but PMU + charging circuitry are populated)
- IMU: QMI8658 @ 0x6B (same chip — initialized for I2C bus health, rotation logic disabled)
- IO expander: **XCA9554 / PCA9554** @ I2C 0x20. Gates LCD_RST, TP_RST, audio amp enable, and reads the PWR button. **`io_expander_init()` MUST run before `gfx->begin()` or `ft3168_init()`** — otherwise display/touch stay in reset and silently fail. PWR button is on EXIO4, active HIGH (verified empirically with the deleted `iox` serial debug command).
- Orientation: **fixed at 0°**. IMU auto-rotation is off (`BOARD_HAS_ROTATION 0`).
- Buttons: GPIO 0 (BOOT → Space/voice-mode), XCA9554 EXIO4 (PWR → brightness; on the stage → next scene; hold → pair). **No third button** (GPIO 18 button doesn't exist on this board).

### AMOLED-1.8 (C6) — `waveshare_amoled_18_c6`
ESP32-C6 sibling of the S3 1.8: same 368×448 SH8601 panel + FocalTech touch, different SoC and GPIO map. **All pins/edges below verified on hardware via temporary GPIO/IRQ scans, since Waveshare's wiki publishes no pin table and the third-party BSP's numbers were partly wrong.**
- Display: **SH8601** AMOLED via QSPI (CS=5, SCLK=0, SDIO0..3=1..4, no MCU reset pin — internal POR; effective reset is the TCA9554 power-cycle). Stock `Arduino_SH8601` init (no vendor-register patch — that's only needed on the C6 2.16).
- Touch: **FT3168** (some units FT6146) @ I2C 0x38, INT=15. Same inline FocalTech reader as the S3 1.8 (regs 0x02..0x06); no reset pin (gated by TCA9554 touch power).
- I2C bus: SDA=8, SCL=7 (shared by TCA9554, AXP2101, FT3168, QMI8658, PCF85063 RTC, ES8311 codec).
- IO expander: **TCA9554 / PCA9554** @ 0x20 — here it gates **power**, not reset: **P4 = display power, P5 = touch power, P7 = audio amp**. `io_expander_init()` runs the documented power-on sequence (P4/P5 LOW → 200 ms → HIGH) and **MUST run before `display_hal_init()`** or the panel stays unpowered. Amp (P7) left off (no audio path).
- PMU: AXP2101 @ 0x34 (owned by `power.cpp`, not `board_init` — LCD isn't on an ALDO rail here).
- IMU: QMI8658 @ 0x6B (init'd for bus health, rotation disabled).
- Orientation: **fixed at 0°**, no rotation (no PSRAM headroom).
- Buttons: **GPIO 9** (BOOT → Space/voice-mode, active LOW — *not* the docs' GPIO 0/9 guess; confirmed by scan), **AXP2101 PKEY** (PWR → brightness; on the stage → next scene; hold → pair). The PKEY **SHORT-press IRQ fires on release** — that's the edge `power.cpp` acts on. No secondary button.

### AMOLED-2.06 (watch form factor) — `waveshare_amoled_206`
- Display: **CO5300** AMOLED via QSPI (CS=12, **SCLK=11** ← same as 1.8, SDIO0..3=4..7, RST=8 direct GPIO). 410×502 portrait. Requires **`col_offset1 = 23`** in the `Arduino_CO5300` constructor — the panel's visible viewport sits at a 22–23 column offset inside the controller's internal RAM. Without it, a vertical strip of stale/garbage content shows through on the right edge (23 was picked empirically for centering; Waveshare's reference library uses 22). The 2.16 dodges this because its 480×480 viewport fills the controller's RAM.
- Touch: **FT3168** via I2C (SDA=15, SCL=14, **INT=38, RST=9** direct GPIO, addr=0x38). Same inline FocalTech reader as the 1.8 port (no GPLv3 `Arduino_DriveBus` dependency). Coordinates verified end-to-end with the BLE reset zone.
- PMU: AXP2101 @ 0x34 (same chip as 2.16/1.8 — `XPowersLib` reused). PWR button routes through AXP PKEY IRQs (short / long / positive), same path as the 2.16 — no IO expander.
- IMU: QMI8658 @ 0x6B (initialized for I2C bus health; rotation logic disabled — fixed watch enclosure orientation).
- RTC: **PCF85063** on the same I2C bus, powered through AXP2101 for retention. Not used by Clawdmeter but present for future features.
- Audio codec: **ES8311** + ES7210 ADC on the same I2C bus. The amp path is unverified on this board, so `sound.cpp` no-ops (same posture as the C6 1.8) — the shared `chime.cpp` engine is ready to wire up once it's tested on hardware.
- **No IO expander** despite the Waveshare wiki FAQ implying one. The schematic shows Key3/PWR wired directly to AXP2101 PWRON; touch reset and display reset are direct GPIOs. `board_init()` pulses LCD_RESET (GPIO 8) and TP_RESET (GPIO 9) before display/touch HAL init.
- Buttons: GPIO 0 (BOOT → Space/voice-mode), AXP PKEY (PWR → brightness; on the stage → next scene; hold-to-pair). **No third button**.
- Flash: 32 MB. Uses `default_32MB.csv` partition table.

### LCD-4 — `waveshare_lcd_4`
- Display: **ST7701** 480×480 RGB parallel (DE=40, VSYNC=39, HSYNC=38, PCLK=41, R0-4=46/3/8/18/17, G0-5=14/13/12/11/10/9, B0-4=5/45/48/47/21); ST7701 init via SW SPI (CS=42, SCK=2, MOSI=1).
- Touch: **GT911** via I2C (SDA=15, SCL=7), polled (wiki INT=GPIO 16 unused). Probe 0x5D then 0x14.
- IO expander: **addr 0x24** (fallback 0x20) on the same I2C bus — must init before `gfx->begin()` (output 0xFF, config 0x3A). Backlight is expander pin 2.
- No PMU / IMU. Buttons: GPIO 0 only (BOOT → Space/PTT). KEY/PWR is EN/RST (hardware reset). GPIO 18 is display R3.
- No PWR button (`has_pwr_button = false`): the pairing hint says to pair from the computer, and a 5 s touch hold on it clears the bonds (`pair_hold.{h,cpp}`, native-tested; `SIM_NO_PWR=1` in the sim). The `unpair` serial command (reply `{"unpair":true}`) clears them on any board.
- RGB tearing fix: pass `bounce_buffer_size_px = LCD_WIDTH * 10` to `Arduino_ESP32RGBPanel`. Do not call `rgbpanel->getFrameBuffer()` after `gfx->begin()`.

## Architecture

```text
firmware/src/
  hal/                      — board-agnostic interfaces: board_caps, display, touch, input, power, imu, sound, board_info
  boards/<name>/            — one folder per board (see list above), plus sim/ and template/
  main.cpp                  — setup() + loop(): BLE payload → payload_parse → signals_ingest → director; buttons; serial commands. Zero #ifdef BOARD_*
  payload.{h,cpp}           — pure JSON → UsageData parser (native-tested)
  signals.{h,cpp}           — fast clock (a / e+es / n, rp) + feeds usage_rate; returns SignalsUpdate (native-tested)
  usage_rate.{h,cpp}        — slow clock: 15-min window of real samples (rp dropped), %/h of the 5-h session → Idle/Normal/Active/Heavy (<5/<10/<20/≥20 %/h); activity lifts the group (3-min TTL); reset = drop > 5 points
  clawd_director.{h,cpp}    — owns the rooms (meters / stage / face) and runs the beats (home, corner, show, screensaver, face-home, streak-home, attention)
  clawd_policy.{h,cpp}      — pure decision rules + ClawdTuning (all timings); clawd_tuning_fast() for SIM_FAST
  mascot.{h,cpp}            — Clawd on the meters: corner seat, idle acts, lurk trips, full-size attention sprite
  splash.{h,cpp}            — the stage: 60×60-cell performer engine (walk, perform, groups)
  face.{h,cpp}, streak.{h,cpp} — screensaver face / curtain, and the orange streak transition (LVGL objects on lv_layer_top)
  clawd_anim.{h,cpp}        — only TU that includes splash_animations.h (~400 KB, generated, do not hand-edit)
  ui.{h,cpp}                — the meters only: cards, clock, status line, battery, seat; stale data dims to 40 % opa. compute_layout() from board_caps() (H >= 460 → large, else compact; content_y 100 / 96)
  minimal_mode.{h,cpp}      — one-room mode from BoardCaps.minimal_ui only; no user or NVS override
  idle.{h,cpp}, brightness.{h,cpp} — 30-min idle fade / sleep; PWR brightness levels in NVS
  ble.{h,cpp}               — NimBLE peripheral: data service + HID keyboard
  ding.{h,cpp}, chime.{h,cpp} — the one sound: a synthesized ding at C#5 (0.7 ms attack, 9 ms strike into a 0.24 s ring, a short noise click; native-tested, mirrored by the desktop's chime.rs) for alerts and resets, streamed by the ES8311 engine
  data.h, icons.h, logo.h, clawd_still.h, font_*.c
firmware/test/              — native unit tests: test_payload, test_signals, test_usage_rate, test_clawd_policy, test_splash_geometry
firmware/sim/               — scenario.jsonl (default), director.jsonl (every beat), director-resume.jsonl (interruptions)
desktop/                    — Rust workspace (see "Desktop app")
docs/                       — clawd-director.md (behaviour), daemon-parity.md (old daemons → app), fonts.md, porting/
tools/                      — convert_official_clawd.js, png_to_lvgl.js; tools/ci/ (release + manifest scripts, unittest'd)
```

Each board folder contains: `board.h` (pins, I2C addresses, `BOARD_HAS_*` flags),
`board_init.cpp` (Wire.begin + any IO expander), `display.cpp`, `touch.cpp`,
`input.cpp`, `power.cpp`, `imu.cpp`, `caps.cpp` (the `BoardCaps` instance), plus
any board-private hardware drivers (e.g. `io_expander.{h,cpp}` on AMOLED-1.8).
PlatformIO's `build_src_filter` includes shared code + one board's folder per env.

**Boot and behaviour.** The device boots to the meters (`director_init`). A tap
anywhere means "show me the meters" (streak home from the stage, face drops);
a tap *on* the meters is tapping the aquarium (corner reaction, and at most
once a minute the `peek` beat: he ducks out and the face looks toward the tap).
The stage is earned: a show after 5 min of activity level 3 (≤ 1 per 15 min,
scene from the usage-only base group floored at Active), a screensaver after
10 quiet min (face; calm-scene dips), attention on `m`. Full state machine,
tunables and C6 differences: `docs/clawd-director.md`. Without hooks
(`signals_hooks_seen()` false) there are no shows, and the screensaver needs a
window that hasn't started (`sr < 0`).

**C6 corner mascot animates** (buffer for the largest corner act, ~23 KB, in
internal RAM); C6 has no full-size sprite, so no lurk trip and attention is
message-only.

## Payload (device ← app)

One JSON object per write to RX (`4c41555a-…-0002`, firmware buffer 512 B),
always a full update. `s sr w wr st acct tp pd rd t tf c ok` (usage; `sr`/`wr`
-1 = not started/unknown; `acct:"ent"` = spend-capped account: `s` is spend %,
`tp/pd/rd` billing period), `b m x` (attention: alert chime, ASCII message, clear),
`a e es n` (fast clock: activity 0–3, event `p`/`s`/`b` + sequence, active
sessions), `rp:1` (heartbeat replay; rate math skips it). `{"ok":false}` = no
data. The device notifies `0x01` on REQ (`…-0004`) when it has no data yet.
TX (`…-0003`) notifies `{"dismiss":true}` when an alert ends on the device
through a tap or its 2-minute timeout. The app clears the alert it sent;
it does not echo a clear back. Uncleared alerts expire in the app after
2 minutes, like the device, or 30 seconds for a test alert. App expiry sends a clear if the
alert was sent to the device.
Key sets are cross-checked by `clawdmeter-core` tests against `doc["…"]` in
`firmware/src/*.cpp`.

## Build / flash

```bash
pio run -d firmware                                                             # default_envs: all 7 hardware envs + sim
pio run -d firmware -e waveshare_amoled_216                                     # one board
pio run -d firmware -e waveshare_amoled_216 -t upload --upload-port /dev/ttyACM0   # flash (macOS: /dev/cu.usbmodem*)
pio test -d firmware -e native                                                  # host unit tests — always pass -e native
```

Envs: `waveshare_amoled_216`, `waveshare_amoled_18`, `waveshare_amoled_216_c6`,
`waveshare_amoled_18_c6`, `waveshare_amoled_206`, `waveshare_lcd_154`,
`waveshare_lcd_4`, `sim`, `native`. A bare `pio test` runs over `default_envs`
(hardware + sim), so pass `-e native`. C6 boards: a "This chip is ESP32-C6,
not ESP32-S3" error means you picked an S3 env.

If `pio` isn't on PATH: `~/.platformio/penv/bin/pio` (Linux/macOS) or `brew install platformio`.

## Desktop simulator (`-e sim`) — develop UI without hardware

```bash
sudo apt install libsdl2-dev   # once (macOS: brew install sdl2)
pio run -d firmware -e sim && (cd firmware && .pio/build/sim/program)
```

An SDL2 window stands in for the 480×480 panel; the **full firmware loop runs
unmodified** (director, meters, idle fade, pair gesture, payload/signals,
rate math, chime logic). Only `ble.cpp`/`chime.cpp` are swapped for stubs.
`boards/sim/` implements the HAL against SDL2, Arduino shims live in
`boards/sim/shim/`, and `ble_sim.cpp` plays back payloads from
`firmware/sim/scenario.jsonl` (one JSON line per state + optional
`name`/`hold_ms`; override with `SIM_SCENARIO=<path>`). Run from `firmware/`.

Keys (full map in `boards/sim/board.h`): mouse = touch · space = play/pause ·
←/→ step · 1-9 jump · d BLE link · b/n BOOT/secondary · p PWR · c/u/-/=
charging/USB-only/battery · e/t prompt/stop event · a activity 0→3 · m alert ·
s screenshot BMP · esc quit.

Env vars: `SIM_FAST=1` (minutes → seconds via `clawd_tuning_fast()`),
`SIM_TRACE=1` (per-step director trace), `SIM_SHOTS="ms:path,…"` (screenshots,
exits after the last), `SIM_TAPS="ms@x,y;…"` (`ms@x,y+hold_ms` for a touch hold),
`SIM_NO_PWR=1` (a board without a PWR button, like LCD-4), `SIM_CMDS="ms:cmd;…"` (scripted
serial commands; `pwr`, `prompt`, `stop`, `alert` stand for the button/keys),
`SIM_MINIMAL=1` (sets the simulator's `BoardCaps.minimal_ui`), `SIM_LCD_W`/`SIM_LCD_H` (other panel sizes; use a separate
`PLATFORMIO_BUILD_DIR` per size, never the scratchpad root),
`SIM_AUTOSHOT_MS` + `SIM_AUTOSHOT_PATH` with `SDL_VIDEODRIVER=dummy` for
headless CI shots.

Walkthroughs:

```bash
cd firmware
SIM_FAST=1 SIM_SCENARIO=sim/director.jsonl .pio/build/sim/program
#   notice ~3 s, show ~14 s, screensaver ~53 s, wake ~90 s, attention ~97–110 s, reset joy ~115 s
SIM_FAST=1 SIM_SCENARIO=sim/director-resume.jsonl \
  SIM_CMDS="2500:clawd lurk;5200:prompt;34000:alert;67500:pwr;80000:prompt" .pio/build/sim/program
```

Reference captures: `screenshots/director/01–34`. **The sim renders with
desktop LVGL and fake data — always do a final check on real hardware before
merging panel-related changes** (col offsets, rotation, rounding live in the
hardware boards, not shared code).

## QA your own UI changes — don't ask the user

The firmware has serial commands: `screenshot` (dumps the LVGL framebuffer;
`./screenshot.sh out.png [port]` turns it into a PNG sized to the panel),
`info` (env/version/rev/link JSON), `perf` (redraw cost of the last face rise / settle / drop / streak), `chime` (plays the ding; `buzz` still works), `chime <gain> [volume]` (tune a board's speaker until reboot: gain 0..1, ES8311 volume 0..100; the 2.16 ships 0.7 / 77, tuned by ear; the 1.8 and LCD-1.54 ship 0.7 / 75, untested by ear), `unpair` (clear bonds, re-advertise), `ble` (link state, bonds, owner, peers), `disconnect` (drop every BLE link), and
`clawd <show|screensaver|attention|joy|lurk|home>` to force a beat. **Use
`clawd <beat>` + `screenshot` on every UI iteration** and Read the PNG; there
is no need to edit the boot screen in `main.cpp` any more. C6 boards have no
`screenshot` (no PSRAM for the snapshot), so eyeball those on hardware.

## Desktop app (`desktop/`)

Rust workspace (edition 2021, MSRV 1.88 for the library crates, 1.90 for `clawdmeter-app`; `cd desktop`):

- `crates/clawdmeter-core` — pure logic: `config` (the `key = value` file, edited in place), `usage` (run + parse `claude --output-format json -p /usage`: `Current session/week` lines, `Spend limit:` lines, overage header, reset dates with optional year), `events` (tail `events.log`), `attention`, `cadence` (when to poll), `selector` (which account), `payload`, `rate`, `engine` (I/O-free state machine: `next_write` / `mark_written`), `autostart` (systemd / XDG / LaunchAgent / Run key, legacy daemon detection + `disable_legacy`), `platform`. Fixtures in `tests/fixtures/` are real or CLI-source-derived `/usage` outputs.
- `crates/clawdmeter-ble` — btleplug link: only OS-known devices (custom service, or HID + exact name), address cache, REQ subscription, zombie-write breaker, BlueZ trust, macOS stale-bond unpair via blueutil, Windows bonded-address fallback (PnP / `CLAWDMETER_BLE_ADDRESS`).
- `crates/clawdmeter-flash` — release `manifest.json` download, board matching by chip + flash size, espflash writes.
- `crates/clawdmeter-app` — the `clawdmeter` binary: tray (`tray/`, ksni on Linux), the app window (`ui/`), `daemon.rs` (drives the engine on its own thread: 0.5 s tick, config re-read 5 s), `hooks/` (`clawdmeter hook alert|clear|event` + the settings.json installer), `ipc.rs` (single-instance lock, status file, command queues, window lock), `onboarding.rs` (`onboarding_done` in the config), `boards.rs` (names, buy links, illustrations).

**One window, one process.** GPUI 0.2.2 ends its event loop when the last window closes on Linux/Windows, so the tray process never runs GPUI: the window is a `clawdmeter --window [PAGE]` child. `ui/app.rs` is its root: a sidebar (Overview, Device, Flash, Accounts, Hooks, Settings) or the setup wizard (`ui/setup.rs`) filling the window. Pages are `crate::Page` (`Page::parse` also takes the old `background`/`flasher`/`status`/`alerts` names). Only one window process runs (`<state>/window.lock`); the tray, or a second `--window` launch, appends `show <page>` to `<state>/window-commands` and the window switches page and raises itself. Child views ask for another page by emitting `ui::Navigate`. The flasher (`ui/flasher.rs`) only touches USB while it's on screen (`set_active`), the Device page only probes while shown, and the sidebar locks while a board is being written, so nothing competes for the serial port. The wizard reuses the flasher (`FlashMode::Setup`: no step header, up-to-date boards can skip) and the Hooks page (`SettingsView::show(Page::Hooks, embedded)`). First run = no `onboarding_done` and no cached BLE address; the tray opens the window on `setup` then.

**Board art and catalog entries are required for new ports.** Create an
original SVG at `desktop/crates/clawdmeter-app/assets/boards/<env>.svg`:
a 3/4 view drawn from the real product, matching the existing illustrations.
Product photos are reference material, not artwork to copy or embed. Add
the display name and buy link to `desktop/crates/clawdmeter-app/src/boards.rs`
and the README's **Supported boards** table, and register the SVG in
`desktop/crates/clawdmeter-app/src/ui/mod.rs`. Waveshare links include
`?aff_id=149786` (use `&aff_id=149786` with an existing query); other vendors
use plain product links. The PR should include the buy link and preferably
a photo of the board running Clawdmeter; state if a photo is unavailable.

Board art is bundled by `ui::Assets` and drawn with
`img("boards/<env>.svg")`. Use `img()`, not `svg()`: `svg()` renders a
monochrome mask; `img()` loads the embedded SVG through resvg in full colour.
`boards::tests` checks the board table and asset registration.

Look: neutral greys in the Claude desktop app's family (`ui/theme.rs`), Clawd orange only for Clawd and primary actions; notices are neutral unless they report a problem. Window frames (`ui/decorations.rs`): native everywhere the platform draws one (macOS/Windows title bar, X11 WM frame, Wayland xdg-decoration on KDE/Sway/…); on a Wayland compositor without xdg-decoration (GNOME) the app asks for client decorations and `ui::chrome` draws an Adwaita-style header bar. gpui 0.2.2 records "server" even when the compositor has no decoration manager, so `decorations::wanted()` probes the Wayland registry itself. `CLAWDMETER_DECORATIONS=client|server` overrides (screenshots of the header bar under X11).

**UI work without hardware:** `cargo run -p clawdmeter-app --example demo -- --page device` (also `--setup welcome|board|flash|pair|hooks|done`, `--board none|fresh|current|old`, `--flash-to identify|firmware|flash|done`, `--status off`, `--hooks-preview`, `--shop`, `--accounts retrying`, `--refresh wait|checking`, `--advanced`) opens the window with a fake board, a fake live status and a local release. Point `HOME`/`XDG_CACHE_HOME` at a scratch dir; it writes there. Screenshots: run it under X11 (`env -u WAYLAND_DISPLAY DISPLAY=:0`), find the client window with `xwininfo -root -tree | grep '"Clawdmeter": ("clawdmeter"'` and `import -window <id> out.png`. The set in `desktop/docs/screenshots/` is made this way with a scratch `HOME` and the demo binary copied into it (hook commands in the preview show the executable's path), so no personal paths show; wait ~5 s before capturing, since on X11 the dark appearance arrives from the portal after the first frames. `overview-gnome-header.png` uses `CLAWDMETER_DECORATIONS=client`. GPUI gotcha: don't `observe_window_appearance` on X11 (0.2.2 redraws inside the portal callback and panics); the root polls the appearance instead.

Run modes: no args = tray app (logs to `<state>/daemon.log`);
`--daemon` headless; `--once` (poll + print payload, no BLE); `--window
[PAGE] [--manifest PATH]`; `--ports`; `--info PORT`; `hook MODE [event]`. Env:
`CLAWDMETER_LOG`, `CLAWDMETER_NO_TRAY`, `POLL_INTERVAL`, `CLAUDE_CONFIG_DIR`
(default account), `CLAWDMETER_BLE_ADDRESS`.

**Launch paths** (`clawdmeter-core::launch`): hooks and Start at login write a stable path (`$APPIMAGE`, the Nix profile link, else `current_exe`). Hooks under an AppImage run a copy extracted to `<state>/appimage/` (renewed when the AppImage changes) so every hook event doesn't FUSE-mount it; Start at login keeps `$APPIMAGE`. Installs refuse to write paths that won't survive (macOS disk image or AppTranslocation, Windows temp/zip folders) and existing entries pointing there show as stale.

State lives in one dir, `~/.config/clawdmeter/` (Windows:
`%LOCALAPPDATA%\Clawdmeter\`): `config`, `events.log`, `attention`,
`clear-<sid>`, `ble-address`, `daemon.log`, app state/locks. Config keys:
`config_dirs`, `poll_interval`, `heartbeat_interval`, `reset_chime` and
`alert_chime` (device; the old `chime` / `beep` keys are read when these are
absent), `desktop_chime` (the same ding on this computer via rodio, ALSA on
Linux; `chime.rs`), `clock`, `claude_path`, `release_repo`, `update_check`, `onboarding_done`,
`alert_on` (alert kinds the daemon lets through: the hook records `k` from
`hook_event_name` / `notification_type`, `engine.on_attention` drops the
rest; default all but `idle`).

Usage sources (per account, `probe.rs`, `oauth.rs`): while the account's
5-hour window is known to be running, a probe (`claude -p --model haiku
--output-format stream-json --verbose --no-session-persistence
--strict-mcp-config --tools ""`, hooks and auto memory off, cwd = state dir)
reads the `rate_limit_event` headers. Otherwise (and on a probe miss, unless
backing off) the idle source: `GET https://api.anthropic.com/api/oauth/usage`
with the account's OAuth token, read-only (macOS: the Keychain first,
`Claude Code-credentials` plus `-<sha256(config dir)[..8]>` for a non-default
dir, as Claude Code names it; then `<config dir>/.credentials.json`; the first
unexpired token wins). User-Agent `clawdmeter/<version>`. **Clawdmeter never
refreshes the token**: on an expired or rejected token it falls back to
`claude -p /usage` (Claude Code refreshes its own token; that doesn't start a
window), under the same backoff, and `/usage` also runs when no token is
readable. Only 429-type misses persist as blocks ("Usage paused by
Anthropic"); probe misses don't.
**The probe must never start a window** (owner's rule): `choose_source` allows
it only when the last known five-hour reset is > 5 min away, or a
prompt/tool event from that account (events.log carries the config dir, from
the hook input's `transcript_path`) came after it, within 5 h. A window that
ends with no activity since becomes "not started" with no call; anything
uncertain (first run, unattributed activity, near a reset) uses the idle
source, which sends no message. Spend-capped accounts always use it. `claude`
is found via `claude_path` → `PATH`/install dirs → the newest
Claude-desktop-bundled build (`<userData>/claude-code/<version>/<hash>/claude`;
verified on disk on Linux, the macOS/Windows variants come from the desktop
app's code and Electron's userData paths). With no `claude` at all there is no
probe: the endpoint answers every 12 min, window or not.

Cadence (`cadence.rs`): with a window known to be running (the probe): shown
account prompt → poll now, then 3 min while working; ~10 s after a Stop; 5 min
with no hooks or between a Stop and 10 quiet min; 15 min after that; other
accounts every 10 min. With no window known (or before the first reading, or
without `claude`): the idle source every 12 min, not sped up by prompts; a
known reset wakes the account once. No account is polled twice within 2 min,
whatever asked (prompt, Stop, device REQ, tray "Refresh now").
Backoff (`backoff.rs`, idle source only): a 429 from the endpoint parks the
account for exactly its `retry-after`; other transient misses (no 429 header,
offline, `/usage` printing no `Current …` lines, timeouts) park it 10 → 20 →
30 min, reset on success; nothing bypasses it (Refresh now, device REQ and
prompts included; the probe is a different path and stays allowed). Calls
during a block make the server lengthen it (589 → 1353 → 2721 → 3600 s
seen), so blocks are saved to `<state>/usage-blocks.json` and restored at
start-up (`--once` honours them too); the Overview and Accounts page say
"Usage paused by Anthropic until HH:MM". A parked shown account stays
shown with its last values. Heartbeat replays (`rp:1`) every
`heartbeat_interval` (60 s) keep the firmware's 90 s `DATA_FRESH_MS` open, and
stop after 3 consecutive misses so the device shows stale. A failed poll is
never a zero.

```bash
cargo fmt --all --check
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo test --workspace --locked
cargo clippy -p clawdmeter-app --target x86_64-pc-windows-gnu -- -D warnings   # Windows cfg check from Linux
cargo run -p clawdmeter-app -- --once                                        # sanity: real usage (idle source) → payload
```

Linux build deps: see `.github/actions/linux-desktop-deps/action.yml`. GPUI
UI tests use `gpui` `test-support` (`#[gpui::test]`); hardware is faked
behind `FlashHardware`. **Tests must never run a real `systemctl`,
`launchctl` or `reg`** against the developer's machine (use the injectable
`autostart::disable_legacy_with`).

## Pull requests

Write for a reviewer seeing the change for the first time. Use
[the PR template](.github/pull_request_template.md).

- Describe the end state: behavior, structure, and relevant file paths.
  Skip the session history, abandoned approaches, and checks already gated
  by CI. Keep the description proportional to the change.
- Include screenshots or pictures when UI or device output changes.
  For device output, use serial captures from `./screenshot.sh` or photos.
  Label simulator captures as simulator output. Add a Mermaid diagram when
  it makes flow, state, or architecture easier to understand.
- Include **Hardware testing**: name the exact boards/revisions tested on
  real hardware and what was verified, or write **not tested on hardware**.
- Include **Human involvement**: state how much the human author reviewed
  and QA'd. For example: “Agent-written; human reviewed the diff and tested
  pairing and touch on an ESP32-S3-Touch-AMOLED-2.16.” If none, write
  **not reviewed by a human**. Never infer human review from an approval to
  proceed; report only confirmed involvement.
- For a new board, include its buy link and preferably an in-person photo
  of it running Clawdmeter. State if no photo is available. The original
  SVG and app/README catalog entries are part of the port, not follow-up work.

## CI / release

- `.github/workflows/firmware.yml` — every hardware env built + merged image artifacts, `pio test` on native envs, sim autoshot, `tools/ci` unittests.
- `.github/workflows/desktop.yml` — fmt + clippy + workspace tests (Linux), core tests on macOS/Windows.
- `.github/workflows/release.yml` — on `vX.Y.Z[-pre]` tags: firmware `<env>-<tag>.bin` (merged, offset 0), `manifest.json` (`tools/ci/make_manifest.py`; new boards need a `DISPLAY_NAMES` entry), `webtools-<env>.json`, desktop archives built with `CLAWDMETER_VERSION=${tag#v}` (Linux binary, macOS ad-hoc-signed universal `Clawdmeter.app` with `NSBluetoothAlwaysUsageDescription` + `LSUIElement`, Windows exe), installers (`.dmg` of the same app via hdiutil, per-user `.msi` from `tools/packaging/wix/clawdmeter.wxs` with WiX 5, `.deb` via cargo-deb from `[package.metadata.deb]` in the app crate, `.AppImage` via `tools/packaging/appimage.sh` on ubuntu-22.04 — host Vulkan/Wayland/D-Bus, nothing GPU-side bundled), `SHA256SUMS`. Nothing is signed beyond the ad-hoc macOS signature. Hyphenated tags are prereleases.
- Packaging assets: `tools/packaging/icons/` (PNG/ICO/ICNS rendered from the official idle Clawd by `tools/packaging/make_icons.py`; committed, and the release fails if they drift from the firmware art), `tools/packaging/linux/clawdmeter.desktop` (app_id `clawdmeter`). Local check: `cargo deb -p clawdmeter-app --no-build` in `desktop/` and `tools/packaging/appimage.sh <bin> <out> <workdir>`.
- `flake.nix` (Linux only): `nix build .#clawdmeter` (rustPlatform, Vulkan/Wayland added to the rpath rather than wrapped, because the app writes its own path into hooks/autostart) and `nix develop` (Rust + platformio + SDL2). Bump with `nix flake update`.
- Lint workflows with `actionlint` (+ shellcheck) before pushing changes to `.github/`.

## Critical gotchas

1. **CO5300 cannot rotate.** Its MADCTL only supports axis flips, not column/row exchange. Rotation is done by **CPU pixel remapping inside `display_hal_draw_bitmap`** in `boards/waveshare_amoled_216/display.cpp`. We use **PARTIAL render mode with strip rotation** (small 480×40 strips, fast). On rotation change → AMOLED brightness flash → force redraw (handled inside `display_hal_tick`).
2. **OPI PSRAM** required on S3: `board_build.arduino.memory_type = qio_opi` in platformio.ini. Without this, `MALLOC_CAP_SPIRAM` returns NULL and the screen is black.
3. **pioarduino platform required.** GFX Library for Arduino needs Arduino Core 3.x (`esp32-hal-periman.h`), not the 2.x that standard `espressif32` ships. We pin `pioarduino/platform-espressif32` 55.03.38-1.
4. **LVGL 9 font patching.** `lv_font_conv` outputs LVGL 8 format. Must remove `#if LVGL_VERSION_MAJOR >= 8` guards, drop `.cache` field, add `.release_glyph`, `.kerning`, `.static_bitmap`, `.fallback`, `.user_data`. Without patching, fonts render invisible. Full regeneration recipe: `docs/fonts.md`.
5. **Touch reading is centralized inside each board's `touch.cpp`.** The HAL `touch_hal_read()` is called once per loop from `my_touch_cb`; the board's implementation owns its latched `touch_pressed/x/y` state. Don't call the underlying controller from anywhere else — CST9220's `getPoint()` etc. do a full I2C transaction and concurrent callers consume each other's data.
6. **Even-aligned flush regions.** `display_hal_round_area` (called from `rounder_cb`) is what each board uses to enforce this. Required on CO5300, harmless on SH8601.
7. **Touch axis swap/mirror is per-board.** The 2.16's CST9220 needs `setSwapXY(true)` + `setMirrorXY(true, false)` — applied inside `boards/waveshare_amoled_216/touch.cpp::touch_hal_init()`. New ports apply their own.
8. **LVGL RGB565A8 is planar.** `w*h` RGB565 pixels followed by `w*h` alpha bytes; `data_size = w*h*3`, `stride = w*2`. Use `init_icon_dsc_rgb565a8()` for icons that overlap non-uniform backgrounds (e.g. battery over the stage). Lucide source PNGs are black-on-transparent — converter must tint to white or icons render invisible. See `tools/png_to_lvgl.js`.
9. **Per-board pre-init is `board_init()`.** Each board's `board_init.cpp` brings up `Wire` and any reset-gating IO expander BEFORE `display_hal_init()`. Skipping the IO expander release on AMOLED-1.8 leaves SH8601 + FT3168 in reset and they silently fail to probe. Same for LCD-4: expander @ 0x24 must run before `gfx->begin()` or the ST7701 stays dark.
10. **No `#ifdef BOARD_*` in shared code.** If you're about to add one, you probably want a `BoardCaps` field or a per-board file instead. See `docs/porting/capability-flags.md`.
11. **LCD-4 RGB bounce buffers.** `Arduino_RGB_Display` DMA-scans PSRAM. Pass `bounce_buffer_size_px = LCD_WIDTH * 10` so ESP-IDF allocates SRAM bounce buffers. Do not call `rgbpanel->getFrameBuffer()` after `gfx->begin()` — it constructs a second RGB panel and crashes.
12. **LCD-4 has only one user button (GPIO 0 / BOOT).** GPIO 18 is display R3. KEY/PWR is EN/RST (hardware reset). No brightness cycling, re-pairing is a 5 s touch hold on the pairing hint, and the stage is only reachable through situations (a tap means "meters").
13. **AMOLED-1.8: XCA9554 EXIO2 must stay HIGH.** Pulling it low takes the FT3168 off the I2C bus (IDF reports `ESP_ERR_INVALID_STATE`, which looks like an I2S/driver wedge). Amp enable is GPIO 46 only.
14. **One Clawd on screen, ever.** Corner sprite, full-size sprite and stage performer are never visible together; beats wait for `mascot_away()` / `splash_performer_visible()`. The stage only opens behind the face. Don't add a second creature (the old stale-data cloud is gone).
15. **"No data" is not a 0 % sample.** `signals` only feeds `ok && !rp` payloads to the rate ring and reset detector; keep it that way or the reset chime fires falsely.

## Icons

`tools/png_to_lvgl.js <input.png> <symbol> [W_MACRO] [H_MACRO] [--tint=RRGGBB | --no-tint]` converts an alpha PNG to RGB565A8. Default tint is white (`0xFFFFFF`) — necessary for Lucide PNGs. Splice output into `firmware/src/icons.h` and use `init_icon_dsc_rgb565a8()` in ui.cpp. Currently only the battery icons use this format; the rest are raw RGB565 baked over the panel background, fine because they live inside opaque zones.

## Splash animations

17 official Anthropic Clawd animations (core poses + persona scenes), archived
with full provenance in `research/clawd-official/`. Pipeline:

```bash
node tools/convert_official_clawd.js            # → firmware/src/splash_animations.h
node tools/convert_official_clawd.js --verify DIR   # + per-animation PNGs for eyeballing
```

Requires ImageMagick; Laptop and Soccer convert from their Lottie exports
(crisp) rather than GIFs. Frames are bounding-box crops on the official 55×37
art stage (ox/oy = stage offset — every animation shares one idle-Clawd
position, so transitions are seamless), one byte per cell into a per-animation
≤16-color RGB565 palette (index 0 = background, true black), per-frame hold ms
with duplicates collapsed (~400 KB total). The converter also: detects each
animation's **loop region** (gait cycles, scene middles; sailing scene's is
located by cross-matching the standalone sailing-loop asset, which is not
emitted), synthesizes the **eyes** (transparent holes in the source GIFs) as
`#141413` ink via border flood-fill, and applies two contrast recolors
(trumpet notes → ivory, magnifier fedora → gray) via component analysis.

`clawd_anim.cpp` is the only reader. The stage (`splash.cpp`) plays intro →
loop → outro on a **60×60 stage** (cell = min(W,H)/60 → 8 px on 480, 6 px on
368, 4 px on 240); the corner seat uses 3 px cells on 480. Walkers translate
with foot-locked per-frame schedules (`clawd_gait_cells`) and mirror when
heading left. On 240×240 an act too tall for the seat (jumping happy) is
swapped for dancing.

**Where the animations come from / finding new ones:** all assets are plain
files under `https://claude.ai/images/clawd/{core,persona}/…` — static assets
are not Cloudflare-gated, only HTML routes are. The asset server returns a
real GIF for a valid filename and an HTML catch-all (both HTTP 200) otherwise,
so **name probing works**: fetch `Clawd-<Name>.gif` and check the magic bytes.
Seven current animations are referenced by no shipped bundle and were found
exactly this way (Anthropic stages seasonal drops — Soccer appeared for the
World Cup). To hunt for new ones: run `research/clawd-official/fetch.sh`
(extend its probe list), and grep a fresh desktop .deb's `ion-dist/` bundles
for `/images/` paths (`research/clawd-official/CLAUDE.md` documents the full
methodology, including the Lottie sources and the assets-proxy).

## User profile / preferences

See `~/.claude/projects/.../memory/` files for persistent context (user is an embedded-beginner senior dev, brand-conscious, prefers iterative UI refinement, dislikes me authoring my own art when third-party assets are intended). Always read those memory files at session start.
