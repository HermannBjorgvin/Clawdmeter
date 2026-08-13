# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# Project context

ESP32-S3 / ESP32-C6 firmware for a desk-side Claude Code usage monitor. Each
supported board lives in its own `firmware/src/boards/<name>/` folder and is
selected via PlatformIO's `build_src_filter`. Adding a board means dropping in
a new folder + a new `[env:...]` block — `main.cpp`, `ui.cpp`, and `splash.cpp`
never see board-specific code. See [`docs/porting/adding-a-board.md`](docs/porting/adding-a-board.md).

Eight ports today (three SoC families, six panel sizes):

- `boards/waveshare_amoled_216/` — original Waveshare ESP32-S3-Touch-AMOLED-2.16 (CO5300, 480×480 square, CST9220 touch, IMU rotation). Build env: `waveshare_amoled_216`.
- `boards/waveshare_amoled_18/` — Waveshare ESP32-S3-Touch-AMOLED-1.8 (368×448 portrait, XCA9554 IO expander). Build env: `waveshare_amoled_18`. **Two panel revisions are auto-detected at boot** (`board_rev()` in `board_init.cpp`, enum in `board_rev.h`): original = SH8601 display + FT3168 touch (0x38); later = CO5300 display + CST816 touch (0x15). One binary drives both.
- `boards/waveshare_amoled_216_c6/` — Waveshare ESP32-C6-Touch-AMOLED-2.16 (SH8601, 480×480, CST9217 touch). Build env: `waveshare_amoled_216_c6`. ESP32-C6 SoC: single-core RISC-V, **no PSRAM**, BLE 5 only.
- `boards/waveshare_amoled_18_c6/` — Waveshare ESP32-C6-Touch-AMOLED-1.8 (368×448 portrait, SH8601, FT3168 touch, TCA9554 expander). Build env: `waveshare_amoled_18_c6`. Same panel as the S3 1.8 but on the C6 SoC. All subsystems (display, touch, BOOT + PWR buttons, battery, BLE) verified on hardware.
- `boards/waveshare_amoled_206/` — Waveshare ESP32-S3-Touch-AMOLED-2.06 (CO5300, 410×502 watch form factor, FT3168 touch, no IO expander, 32 MB flash, PCF85063 RTC, ES8311 codec). Build env: `waveshare_amoled_206`. Display, touch, battery, IMU init, and BLE verified on hardware; the ES8311 chime path is not wired up (`sound.cpp` no-ops).
- `boards/waveshare_lcd_154/` — Waveshare ESP32-S3-Touch-LCD-1.54 (ST7789, 240×240 square, CST816T touch @ 0x15). Build env: `waveshare_lcd_154`. **The first non-AMOLED port**: a plain 4-wire SPI TFT, not QSPI, and the panel has no brightness command — backlight is LEDC PWM on `LCD_BL`. **No PMU**: battery is an ADC divider on GPIO1 and `BAT_EN` (GPIO2) is a power-hold line that must be driven HIGH early in `board_init()` or the board browns out on battery. Three buttons (BOOT + GPIO5 + a PWR-role GPIO4); ES8311 chime wired up; QMI8658 populated but unused (fixed orientation, no rotation).

- `boards/esp32_devkit_st7789/` — **DIY wiring, not a vendor kit**: classic ESP32 DevKit v1 (WROOM-32) + ST7789V2 240×280 over 4-wire SPI, display-only (no touch/PMU/battery/codec), BOOT + two wired buttons. Build env: `esp32_devkit_st7789`. **The only classic-ESP32 port** (Xtensa LX6, BLE 4.2, no PSRAM, no native USB) — so no `ARDUINO_USB_CDC_ON_BOOT` (Serial is UART0 through the CP2102/CH340) and `huge_app.csv` partitions, since the ~1.7 MB image doesn't fit a 4 MB board's stock 1.25 MB app slot. **Verified on hardware** (2026-08-11): boots, ST7789V2 240×280 renders the splash, BLE advertises, PWR (GPIO 26) cycles the screens. **First touchless board**, so it sets `BOARD_HAS_TOUCH 0` → `caps.has_touch` false → the PWR short press takes over the tap-to-toggle gesture in `main.cpp` (without it the usage screen is unreachable and stays `HIDDEN` forever); animation/brightness cycling is given up here. BOOT/GPIO25 HID and the daemon link are still untested.

- `boards/lilygo_tdisplay/` — LilyGO TTGO T-Display (classic ESP32) + 1.14" ST7789 **135×240 panel, driven landscape as 240×135**. Build env: `lilygo_tdisplay`. A vendor board sharing the DIY DevKit port's shape (classic ESP32, no PSRAM, no native USB, PWM backlight on GPIO 4, `huge_app.csv`), but **the smallest display in the tree** — the splash stage is min(W,H)/60 = **2 px per cell**. Pin map probe-confirmed on the real board (ESP32-D0WDQ6 rev 1.0, 16 MB flash, 0 B PSRAM), matching Arduino_GFX's `LILYGO_T_DISPLAY` block. Two gotchas worth keeping straight: **`LCD_PANEL_W/H` (135×240 native, what the `Arduino_ST7789` constructor wants) is deliberately separate from `LCD_WIDTH/HEIGHT` (240×135, what LVGL and `caps.cpp` publish)** — mixing them draws into a 135-wide window on a 240-wide screen; and the GRAM offsets (52/40/53/40) are passed as-is because `Arduino_TFT::setRotation` picks the right pair per rotation. Unlike the CO5300 AMOLEDs, the ST7789 exchanges rows/columns in hardware, so rotation is one register write and `display_hal_tick` stays empty. **Touchless** (`BOARD_HAS_TOUCH 0`), so PWR = **GPIO 35** cycles screens; GPIO 35 is input-only with no internal pull-up, hence `BTN_PWR_INPUT_ONLY` and `INPUT` (not `INPUT_PULLUP`) in `power.cpp`. BOOT = GPIO 0; the third button is RST wired to EN and invisible to software, so there's no secondary/HID button. Battery is a 2:1 divider on GPIO 34 that reads ~4.2 V on USB with or without a cell — "no battery" and "full battery" are indistinguishable here.

Plus one non-hardware target: `boards/sim/` — **native desktop simulator** (SDL2 window, 480×480, `platform = native`). Build env: `sim`. See "Desktop simulator" below.

**C6 ports have no PSRAM** — shared code gates on `BOARD_HAS_PSRAM` (absent on C6) to use `MALLOC_CAP_INTERNAL` for LVGL/splash buffers, and the `screenshot` serial command is disabled (`LV_USE_SNAPSHOT=0`), so UI changes on a C6 board must be eyeballed on hardware, not auto-captured.

The shared code calls a small HAL (`firmware/src/hal/`) that each board implements: display, touch, input, power, IMU. Optional features are guarded by `BoardCaps` (runtime) and `BOARD_HAS_*` (compile-time) rather than `#ifdef BOARD_*`.

Connects to a host daemon over BLE; daemon polls Anthropic API for usage data. This file is for future Claude Code sessions to bootstrap quickly. Read this first.

## Hardware (critical pins)

### AMOLED-2.16 (original)
- Display: **CO5300** AMOLED via QSPI (CS=12, SCLK=38, SDIO0..3=4..7, RST=2)
- Touch: **CST9220** via I2C (SDA=15, SCL=14, INT=11, addr=0x5A)
- PMU: **AXP2101** on same I2C bus (addr=0x34) — battery, USB VBUS, PWR button IRQ
- IMU: **QMI8658** on same I2C bus (addr=0x6B) — accelerometer for auto-rotation
- Buttons: GPIO 0 (left → Space/voice-mode), GPIO 18 (right → Shift+Tab/mode-toggle), AXP PKEY (middle → cycle screens; on splash → cycle animations)

### AMOLED-1.8 (newer port)
**Two hardware revisions ship under this name; the firmware probes I2C at boot and picks drivers automatically (`board_rev()`):**
- Display: **SH8601** (original) or **CO5300** (later rev) AMOLED via QSPI (CS=12, **SCLK=11** ← different!, SDIO0..3=4..7, RST routed via XCA9554 EXIO1). Both are `Arduino_OLED` subclasses held behind one base pointer in `display.cpp`. The CO5300's 368-wide active area starts at GRAM column 16, so it gets `CO5300_COL_OFFSET 16` to center; SH8601 needs none.
- Touch: **FT3168** @ 0x38 (original) or **CST816** @ 0x15 (later rev), via I2C (SDA=15, SCL=14, INT=21). Both expose the same FocalTech-style data layout at regs 0x02..0x06, so one inline reader in `touch.cpp` serves both — only the address differs. Avoids vendoring the GPLv3 `Arduino_DriveBus` library. Revision is detected by which touch address ACKs (CST816 present ⇒ CO5300 panel).
- PMU: AXP2101 @ 0x34 (same chip as 2.16 — `XPowersLib` reused; battery is an optional kit add-on but PMU + charging circuitry are populated)
- IMU: QMI8658 @ 0x6B (same chip — initialized for I2C bus health, rotation logic disabled)
- IO expander: **XCA9554 / PCA9554** @ I2C 0x20. Gates LCD_RST, TP_RST, audio amp enable, and reads the PWR button. **`io_expander_init()` MUST run before `gfx->begin()` or `ft3168_init()`** — otherwise display/touch stay in reset and silently fail. PWR button is on EXIO4, active HIGH (verified empirically with the deleted `iox` serial debug command).
- Orientation: **fixed at 0°**. IMU auto-rotation is disabled; `rotate_strip()` / `handle_rotation_change()` are excluded via `#ifndef BOARD_AMOLED_18`.
- Buttons: GPIO 0 (BOOT → Space/voice-mode), XCA9554 EXIO4 (PWR → cycle screens; on splash → cycle animations). **No third button** (GPIO 18 button doesn't exist on this board).

### AMOLED-1.8 (C6) — `waveshare_amoled_18_c6`
ESP32-C6 sibling of the S3 1.8: same 368×448 SH8601 panel + FocalTech touch, different SoC and GPIO map. **All pins/edges below verified on hardware via temporary GPIO/IRQ scans, since Waveshare's wiki publishes no pin table and the third-party BSP's numbers were partly wrong.**
- Display: **SH8601** AMOLED via QSPI (CS=5, SCLK=0, SDIO0..3=1..4, no MCU reset pin — internal POR; effective reset is the TCA9554 power-cycle). Stock `Arduino_SH8601` init (no vendor-register patch — that's only needed on the C6 2.16).
- Touch: **FT3168** (some units FT6146) @ I2C 0x38, INT=15. Same inline FocalTech reader as the S3 1.8 (regs 0x02..0x06); no reset pin (gated by TCA9554 touch power).
- I2C bus: SDA=8, SCL=7 (shared by TCA9554, AXP2101, FT3168, QMI8658, PCF85063 RTC, ES8311 codec).
- IO expander: **TCA9554 / PCA9554** @ 0x20 — here it gates **power**, not reset: **P4 = display power, P5 = touch power, P7 = audio amp**. `io_expander_init()` runs the documented power-on sequence (P4/P5 LOW → 200 ms → HIGH) and **MUST run before `display_hal_init()`** or the panel stays unpowered. Amp (P7) left off (no audio path).
- PMU: AXP2101 @ 0x34 (owned by `power.cpp`, not `board_init` — LCD isn't on an ALDO rail here).
- IMU: QMI8658 @ 0x6B (init'd for bus health, rotation disabled).
- Orientation: **fixed at 0°**, no rotation (no PSRAM headroom).
- Buttons: **GPIO 9** (BOOT → Space/voice-mode, active LOW — *not* the docs' GPIO 0/9 guess; confirmed by scan), **AXP2101 PKEY** (PWR → cycle screens; on splash → cycle animations). The PKEY **SHORT-press IRQ fires on release** — that's the edge `power.cpp` acts on. No secondary button.

### AMOLED-2.06 (watch form factor) — `waveshare_amoled_206`
- Display: **CO5300** AMOLED via QSPI (CS=12, **SCLK=11** ← same as 1.8, SDIO0..3=4..7, RST=8 direct GPIO). 410×502 portrait. Requires **`col_offset1 = 23`** in the `Arduino_CO5300` constructor — the panel's visible viewport sits at a 22–23 column offset inside the controller's internal RAM. Without it, a vertical strip of stale/garbage content shows through on the right edge (23 was picked empirically for centering; Waveshare's reference library uses 22). The 2.16 dodges this because its 480×480 viewport fills the controller's RAM.
- Touch: **FT3168** via I2C (SDA=15, SCL=14, **INT=38, RST=9** direct GPIO, addr=0x38). Same inline FocalTech reader as the 1.8 port (no GPLv3 `Arduino_DriveBus` dependency). Coordinates verified end-to-end with the BLE reset zone.
- PMU: AXP2101 @ 0x34 (same chip as 2.16/1.8 — `XPowersLib` reused). PWR button routes through AXP PKEY IRQs (short / long / positive), same path as the 2.16 — no IO expander.
- IMU: QMI8658 @ 0x6B (initialized for I2C bus health; rotation logic disabled — fixed watch enclosure orientation).
- RTC: **PCF85063** on the same I2C bus, powered through AXP2101 for retention. Not used by Clawdmeter but present for future features.
- Audio codec: **ES8311** + ES7210 ADC on the same I2C bus. The amp path is unverified on this board, so `sound.cpp` no-ops (same posture as the C6 1.8) — the shared `chime.cpp` engine is ready to wire up once it's tested on hardware.
- **No IO expander** despite the Waveshare wiki FAQ implying one. The schematic shows Key3/PWR wired directly to AXP2101 PWRON; touch reset and display reset are direct GPIOs. `board_init()` pulses LCD_RESET (GPIO 8) and TP_RESET (GPIO 9) before display/touch HAL init.
- Buttons: GPIO 0 (BOOT → Space/voice-mode), AXP PKEY (PWR → cycle screens; hold-to-pair). **No third button**.
- Flash: 32 MB. Uses `default_32MB.csv` partition table.

## Architecture

```text
firmware/src/
  hal/                      — board-agnostic interfaces shared code calls into
    board_caps.h            — runtime BoardCaps struct (W, H, button_count, has_* flags)
    display_hal.h           — init / begin / set_brightness / draw_bitmap / tick / round_area
    touch_hal.h             — init / read(&x, &y, &pressed)
    input_hal.h             — init / is_held(PRIMARY|SECONDARY)
    power_hal.h             — init / tick / battery_pct / is_charging / pwr_pressed (edge)
    imu_hal.h               — init / tick / rotation_quadrant
  boards/
    waveshare_amoled_216/   — CO5300 + CST9220 + AXP PKEY + QMI8658 rotation
    waveshare_amoled_18/    — SH8601 + FT3168 + AXP + XCA9554 (PWR via EXIO4), no rotation
    waveshare_amoled_216_c6/— C6: SH8601 + CST9217 + AXP PKEY, no PSRAM
    waveshare_amoled_18_c6/ — C6: SH8601 + FT3168 + AXP PKEY + TCA9554 (gates power), no PSRAM
    waveshare_amoled_206/   — CO5300 + FT3168 + AXP PKEY, no IO expander, 32 MB, no rotation
    waveshare_lcd_154/      — ST7789 SPI TFT + CST816T + ADC battery (no PMU), PWM backlight
    esp32_devkit_st7789/    — classic ESP32 DIY: ST7789V2 240x280, display-only, touchless
    lilygo_tdisplay/        — classic ESP32 vendor: ST7789 240x135 landscape, touchless, ADC battery
    sim/                    — native desktop simulator: SDL2 + Arduino shims + scenario playback
    template/               — copy this to bootstrap a new port
  main.cpp                  — setup() + loop(): HAL calls only, zero #ifdef BOARD_*
  ui.{h,cpp}                — 3-screen UI (splash, usage, opencode), cycled in enum order by one gesture (tap, or PWR on touchless boards). compute_layout() picks fonts/positions from board_caps() (responsive — current breakpoint: H >= 460 → large, else compact)
  splash.{h,cpp}            — 60×60 pixel-art engine (see "Splash animations")
  splash_geometry.h         — pure cell-size/scale math for the splash stage; no LVGL/Arduino deps so it host-unit-tests (PSRAM → full-size canvas at 1.0x; no PSRAM → 60×60 buffer LVGL upscales, ~7 KB instead of ~460 KB)
  ble.{h,cpp}               — NimBLE peripheral: custom data service + HID keyboard
  idle.{h,cpp}              — idle fade-out/wake. **Owns panel brightness** — never call display_hal_set_brightness directly
  brightness.{h,cpp}        — user brightness level (PWR short-press cycles), persisted to NVS, applied via idle_set_awake_brightness()
  usage_rate.{h,cpp}        — %/min rate of change of session_pct → 4 animation groups (idle/normal/active/heavy); also flags session resets to trigger the chime
  chime.{h,cpp} + es8311.c  — board-agnostic ES8311 + I2S engine; a board's sound.cpp supplies pins/volume/amp_enable. bell_pcm.h is 44.1 kHz stereo — sample_rate must match
  theme.h                   — color tokens (single source of truth for UI colors)
  data.h                    — UsageData struct
  icons.h                   — icon arrays. Battery (5×) are RGB565A8 with alpha; rest are raw RGB565.
  logo.h                    — 80×80 RGB565 logo
  font_*.c                  — pre-compiled LVGL 9 bitmap fonts (Tiempos 56/34, Styrene 48/28/24/20/16/14/12, Mono 32/18)
  splash_animations.h       — generated, do not hand-edit
docs/porting/               — adding-a-board.md, hal-contract.md, capability-flags.md
```

Each board folder contains: `board.h` (pins, I2C addresses, `BOARD_HAS_*` flags),
`board_init.cpp` (Wire.begin + any IO expander), `display.cpp`, `touch.cpp`,
`input.cpp`, `power.cpp`, `imu.cpp`, `caps.cpp` (the `BoardCaps` instance), plus
any board-private hardware drivers (e.g. `io_expander.{h,cpp}` on AMOLED-1.8).
PlatformIO's `build_src_filter` includes shared code + one board's folder per env.

## Build / flash

```bash
pio run -d firmware -e waveshare_amoled_216                                     # build 2.16 (S3, default original)
pio run -d firmware -e waveshare_amoled_18                                      # build 1.8 (S3)
pio run -d firmware -e waveshare_amoled_216_c6                                  # build 2.16 (C6)
pio run -d firmware -e waveshare_amoled_18_c6                                   # build 1.8 (C6)
pio run -d firmware -e waveshare_amoled_206                                     # build 2.06 (S3, watch)
pio run -d firmware -e waveshare_lcd_154                                        # build 1.54 (S3, SPI TFT)
pio run -d firmware -e esp32_devkit_st7789                                      # build DIY ESP32 DevKit v1 + ST7789V2 240x280
pio run -d firmware -e esp32_devkit_st7789 -t upload --upload-port COM5         # flash it (USB-UART bridge, not native USB)
pio run -d firmware -e lilygo_tdisplay                                          # build LilyGO TTGO T-Display (classic ESP32, 240x135)
pio run -d firmware -e lilygo_tdisplay -t upload --upload-port /dev/cu.wchusbserial*  # flash it on macOS (CH9102 bridge; older units are CP2104 → /dev/cu.usbserial-*)
pio run -d firmware -e waveshare_amoled_18 -t upload --upload-port /dev/cu.usbmodem101   # flash 1.8 on macOS
pio run -d firmware -e waveshare_amoled_216 -t upload --upload-port /dev/ttyACM0         # flash 2.16 on Linux
# C6 boards: same native USB-JTAG flashing; flag a chip mismatch ("This chip is ESP32-C6,
# not ESP32-S3") means you picked an S3 env — use a *_c6 env for C6 hardware.
```

If `pio` isn't on PATH: try `~/.platformio/penv/bin/pio` (Linux/macOS pio install) or `brew install platformio` on macOS.

Device path differs by OS: `/dev/cu.usbmodem*` on macOS, `/dev/ttyACM0` on Linux. Both expose the ESP32-S3 native USB-JTAG (no boot-mode dance needed).

Wrappers users run (keep working when you touch envs/pins): `./flash.sh` / `./flash-mac.sh <env> [port]` (env list is scraped from `firmware/platformio.ini`), `./install.sh` (Linux systemd), `./install-mac.sh` (venv + LaunchAgent), `./install-windows.ps1` (venv + tray + autostart).

## Tests

There is **no CI** — run these yourself before claiming done.

```bash
python -m pytest daemon/tests -q                          # host daemon suite (run from repo ROOT: conftest.py puts the repo on sys.path so `import daemon.*` resolves)
python -m pytest daemon/tests/test_freeride.py -x -q      # one file
python -m pytest daemon/tests -q -k decode_hex            # one test
bash daemon/tests/test_bash_token.sh                      # bash daemon's read_token_for() (evals just that function out of the .sh)
cd firmware/test/test_splash_geometry && g++ -std=c++17 -I ../../src test_main.cpp -o t && ./t   # host C++ unit test, no PlatformIO needed
```

Two tests in `test_windows_token.py` are **Linux/WSL-only** (skipped on macOS and native Windows — they spawn the Windows daemon and assert its "WinRT unavailable" warning); everything else mocks `bleak`/`httpx` and runs anywhere. Firmware itself has no on-device test suite — `pio run -e <env>` (compile) + the simulator + `screenshot.sh` are the verification loop.

## Desktop simulator (`-e sim`) — develop UI without hardware

```bash
sudo apt install libsdl2-dev   # once (macOS: brew install sdl2)
pio run -d firmware -e sim && (cd firmware && .pio/build/sim/program)
```

An SDL2 window stands in for the 480×480 panel; the **full firmware loop runs
unmodified** — `main.cpp`, `ui.cpp`, `splash.cpp`, idle fade, pair gesture,
JSON parsing, usage-rate/chime logic. Only `ble.cpp`/`chime.cpp` are swapped
for stubs. How it works: `boards/sim/` implements the HAL against SDL2, thin
Arduino shims live in `boards/sim/shim/` (`millis`/`Serial`→stdio,
`heap_caps`→malloc, in-memory `Preferences`), and `ble_sim.cpp` plays back
daemon payloads from `firmware/sim/scenario.jsonl` (one JSON line per state +
optional `name`/`hold_ms`; override with `SIM_SCENARIO=<path>`).

Controls (full map in `boards/sim/board.h`): mouse = touch · space =
play/pause scenario · ←/→ = step · 1-9 = jump · d = BLE link toggle ·
b/n = BOOT/secondary buttons · p = PWR · c/-/= = charging/battery ·
s = screenshot BMP · esc = quit.

Headless screenshots (works in CI, no display):
`SDL_VIDEODRIVER=dummy SIM_AUTOSHOT_MS=6000 .pio/build/sim/program` saves
`sim-autoshot.bmp` (or `SIM_AUTOSHOT_PATH`) after 6 s and exits. Combine with
the boot-screen swap trick below to capture any screen. **The sim renders with
desktop LVGL and fake data — always do a final check on real hardware before
merging panel-related changes** (col offsets, rotation, rounding live in the
hardware boards, not shared code).

## QA your own UI changes — don't ask the user

The firmware ships a `screenshot` serial command that dumps the LVGL framebuffer. `./screenshot.sh out.png [port]` captures a PNG sized to the active display (480×480 or 368×448). **Use this on every UI iteration** — Read the PNG with the Read tool, verify the change visually, iterate. Script auto-picks the macOS/Linux default port and falls back to pio's bundled Python if pyserial isn't on the system Python.

The boot screen is `SCREEN_SPLASH` and only advances on a physical button press, so a fresh flash will sit on the splash. To screenshot the screen you're actually editing without asking the user to press a button, **temporarily change the default boot screen** in `main.cpp` (search for `ui_show_screen(SCREEN_SPLASH);`) to `SCREEN_USAGE` / `SCREEN_CONTROLLER` / `SCREEN_BLUETOOTH`, do your iteration, then revert before committing.

## Critical gotchas

1. **CO5300 cannot rotate.** Its MADCTL only supports axis flips, not column/row exchange. Rotation is done by **CPU pixel remapping inside `display_hal_draw_bitmap`** in `boards/waveshare_amoled_216/display.cpp`. We use **PARTIAL render mode with strip rotation** (small 480×40 strips, fast). On rotation change → AMOLED brightness flash → force redraw (handled inside `display_hal_tick`).
2. **OPI PSRAM** required: `board_build.arduino.memory_type = qio_opi` in platformio.ini. Without this, `MALLOC_CAP_SPIRAM` returns NULL and the screen is black.
3. **pioarduino platform required.** GFX Library for Arduino needs Arduino Core 3.x (`esp32-hal-periman.h`), not the 2.x that standard `espressif32` ships. We pin `pioarduino/platform-espressif32` 55.03.38-1.
4. **LVGL 9 font patching.** `lv_font_conv` outputs LVGL 8 format. Must remove `#if LVGL_VERSION_MAJOR >= 8` guards, drop `.cache` field, add `.release_glyph`, `.kerning`, `.static_bitmap`, `.fallback`, `.user_data`. Without patching, fonts render invisible. Full regeneration recipe: `docs/fonts.md`.
5. **Touch reading is centralized inside each board's `touch.cpp`.** The HAL `touch_hal_read()` is called once per loop from `my_touch_cb`; the board's implementation owns its latched `touch_pressed/x/y` state. Don't call the underlying controller from anywhere else — CST9220's `getPoint()` etc. do a full I2C transaction and concurrent callers consume each other's data.
6. **Even-aligned flush regions.** `display_hal_round_area` (called from `rounder_cb`) is what each board uses to enforce this. Required on CO5300, harmless on SH8601.
7. **Touch axis swap/mirror is per-board.** The 2.16's CST9220 needs `setSwapXY(true)` + `setMirrorXY(true, false)` — applied inside `boards/waveshare_amoled_216/touch.cpp::touch_hal_init()`. New ports apply their own.
8. **LVGL RGB565A8 is planar.** `w*h` RGB565 pixels followed by `w*h` alpha bytes; `data_size = w*h*3`, `stride = w*2`. Use `init_icon_dsc_rgb565a8()` for icons that overlap non-uniform backgrounds (e.g. battery over splash). Lucide source PNGs are black-on-transparent — converter must tint to white or icons render invisible. See `tools/png_to_lvgl.js`.
9. **Per-board pre-init is `board_init()`.** Each board's `board_init.cpp` brings up `Wire` and any reset-gating IO expander BEFORE `display_hal_init()`. Skipping the IO expander release on AMOLED-1.8 leaves SH8601 + FT3168 in reset and they silently fail to probe.
10. **No `#ifdef BOARD_*` in shared code.** The whole point of the refactor — if you're about to add one, you probably want a `BoardCaps` field or a per-board file instead. See `docs/porting/capability-flags.md`.

## Icons

`tools/png_to_lvgl.js <input.png> <symbol> [W_MACRO] [H_MACRO] [--tint=RRGGBB | --no-tint]` converts an alpha PNG to RGB565A8. Default tint is white (`0xFFFFFF`) — necessary for Lucide PNGs. Splice output into `firmware/src/icons.h` and use `init_icon_dsc_rgb565a8()` in ui.cpp. Currently only the 5 battery icons use this format; the rest are still raw RGB565 baked over the panel background, fine because they live inside opaque zones.

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

The splash engine (`splash.cpp`) plays intro → loop → outro on a **60×60
stage** (`SPLASH_GRID`, cell = min(W,H)/60 → 8 px on 480, 6 px on 368, 4 px on
240): loops hold until released (walk arrival, scene timer, rotation), so
switches always pass through the shared idle pose. Walkers translate with
foot-locked per-frame schedules and mirror when heading left. Usage-rate
groups pick animations by name; the same rate drives the **corner mascot** on
the usage screen (`splash_mascot_*`, PSRAM boards; C6 falls back to the static
`clawd_still.h` icon) — idle stills, rate-scaled acts, and walk-off/lurk/
walk-back trips. Default boot screen.

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

## Recent session highlights

- **Official Clawd art on a 60×60 stage (2026-08-10).** Replaced the hand-made 20×20 pixel art with the 17 archived Anthropic animations, loop-region playback, and the corner mascot; splash sizing moved into the host-testable `splash_geometry.h` so PSRAM-less C6 boards render a 60×60 buffer LVGL upscales.
- **AMOLED-1.8 chime verified on hardware + EXIO2 touch-kill fix (2026-07-13).** The 1.8's `amp_enable` hook drove both GPIO 46 and XCA9554 EXIO2 ("the unused one is harmless") — but pulling EXIO2 low takes the FT3168 off the I2C bus (chip stops ACKing; IDF reports it as `ESP_ERR_INVALID_STATE`, which reads like a driver wedge and cost a long I2S red-herring chase). Amp enable is GPIO 46 only; EXIO2 must stay HIGH. Chime, touch, buttons, and BLE bond persistence all verified on a real 1.8.
- **Device-abstraction refactor (2026-05-18).** All board-conditional code moved out of shared files into `boards/<name>/` and behind a HAL in `hal/`. ~30 `#ifdef BOARD_*` blocks went to zero. UI is responsive via `compute_layout()` driven by `board_caps()`. New ports add a folder + a PlatformIO env — no shared file edits.
- Added second board port: Waveshare AMOLED-1.8 (368×448 portrait, SH8601, FT3168, XCA9554 IO expander).
- Migrated from Panlee SC01 Plus (480×320 IPS) to Waveshare 2.16" AMOLED (480×480 square). Full hardware/library swap.
- Added IMU auto-rotation, battery indicator, USB-state-aware screen switching.
- Added splash screen with scraped pixel-art animations and 3-button physical input layout.
- Fonts and icons re-scaled ~1.9× for the higher-DPI panel.
- All UI margins widened to 20px to clear the rounded display corners.
- Battery icons converted to RGB565A8 alpha so they blend cleanly over the splash animations.

## Daemon / host side

**Three independent daemons, one per host OS — a protocol/behaviour change must land in all three:**

| OS | Entry point | Token source | Service manager |
|---|---|---|---|
| Linux | `daemon/claude-usage-daemon.sh` (bash + bluetoothctl/busctl) | `~/.claude/.credentials.json` | systemd user unit (`systemctl --user start claude-usage-daemon`) |
| macOS | `daemon/claude_usage_daemon.py` (bleak + httpx) | Keychain service `Claude Code-credentials` | LaunchAgent `com.user.claude-usage-daemon.plist` |
| Windows | `daemon/claude_usage_daemon_windows.py` (+ `tray_windows.py`, `autostart_windows.py`) | native-Windows credentials path | tray app + Run-key autostart |

The systemd unit's `ExecStart` is an absolute path to the script — repoint it when switching between a worktree and the main checkout.

**Invariants that hold across all three:**

- **Free-ride tokens.** The daemon NEVER refreshes the OAuth token — Claude Code owns refreshing. The daemon reads whatever access token is stored and, when it's dead, pushes "No data" to the device. Don't reintroduce refresh machinery (it was deliberately deleted; `test_freeride.py` guards this).
- **Read only `claudeAiOauth`.** A real credentials file holds many `accessToken` fields (MCP servers etc.); concatenating them yields a 401 and the device shows 0% forever. Guarded by `test_bash_token.sh`.
- **Never scan by name.** The daemon connects only to a Clawdmeter the host already has paired/connected (bluez lists it, or the Mac/PC is bonded to it) — no LE advertising scan, so it can't grab a stranger's board. The firmware side matches: `ble.cpp` locks to a **single owner** identity address in NVS and prunes foreign bonds; the 3-second PWR hold+release gesture clears the owner so the board can be handed to another machine.
- `POLL_INTERVAL=60`, `TICK=5`. The inner loop wakes every 5 s to detect disconnects fast; polls Anthropic when 60 s elapsed OR when the ESP fires a refresh request.
- **OpenCode Go is a separate, optional source.** `daemon/opencode_usage.py` (shared by both Python daemons; the bash one shells out to it with `--fragment`) feeds the OpenCode screen, which mirrors the Anthropic one because the data mirrors it too. Two local-first sources:
  - **Limits** — `GET https://opencode.ai/zen/go/v1/usage`, authenticated with the key OpenCode already stored in `~/.local/share/opencode/auth.json` (`opencode-go.key`, or `$OPENCODE_API_KEY`). Undocumented — found by probing, since the Zen docs list no usage endpoint — and it returns `rolling`/`weekly`/`monthly`, each with `percent` + ISO `resetsAt`. Becomes `ocs`/`ocsr`, `ocw`/`ocwr`, `ocmo`, `ocst`. **The `User-Agent` header is load-bearing**: Cloudflare fronts opencode.ai and 403s urllib's default `Python-urllib/3.x` with error code 1010; any normal UA passes (this is why an httpx probe succeeds where urllib fails). Uses `urllib`, not `httpx`, because the bash daemon runs this module under the host's bare python3, not the daemons' venv.
  - **Tokens** — read-only stdlib `sqlite3` against `$OPENCODE_DATA_DIR/opencode.db`, whose `session` table carries per-session token counters and `model` (`oct`, `ocm`). **Not** `ccusage`: that parses the older `storage/message/*.json` layout, which current OpenCode versions no longer write, so it silently reports nothing. `time_updated` is epoch **milliseconds** and `model` is a JSON blob (`{"id":…,"providerID":…}`).
  Go is a subscription, so spend is fractions of a cent and is deliberately not displayed — the windows are what run out. No limits → no fields at all → "No data", never a fabricated 0%. Free-ride like the Anthropic side: the stored key is read, never refreshed.

**GATT characteristics on service `4c41555a-...0001`:**

- `...0002` RX — daemon writes JSON usage payload here.
- `...0003` TX — firmware notifies ack/nack (daemon doesn't subscribe).
- `...0004` REQ — firmware fires `0x01` notify in `onSubscribe` if `has_received_data` is false. Daemon subscribes via `setsid bash -c "stdbuf -oL dbus-monitor … | awk …"`; awk drops a flag file the inner loop picks up. See the `feedback_dbus_monitor_pipe` memory for the three subtle gotchas (pipe buffering, busctl-exits race, `wait` blocking on pipeline jobs).
