# Project context

ESP32-S3 firmware for a desk-side Claude Code usage monitor. Each supported
board lives in its own `firmware/src/boards/<name>/` folder and is selected
via PlatformIO's `build_src_filter`. Adding a board means dropping in a new
folder + a new `[env:...]` block — `main.cpp`, `ui.cpp`, and `splash.cpp`
never see board-specific code. See [`docs/porting/adding-a-board.md`](docs/porting/adding-a-board.md).

Three reference ports today:

- `boards/waveshare_amoled_216/` — original Waveshare ESP32-S3-Touch-AMOLED-2.16 (CO5300, 480×480 square, CST9220 touch, IMU rotation). Build env: `waveshare_amoled_216`.
- `boards/waveshare_amoled_18/` — Waveshare ESP32-S3-Touch-AMOLED-1.8 (SH8601, 368×448 portrait, FT3168 touch, XCA9554 IO expander). Build env: `waveshare_amoled_18`.
- `boards/waveshare_amoled_206/` — Waveshare ESP32-S3-Touch-AMOLED-2.06 (CO5300, 410×502 watch form factor, FT3168 touch, no IO expander, 32 MB flash, PCF85063 RTC, ES8311 codec). Build env: `waveshare_amoled_206`.

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
- Display: **SH8601** AMOLED via QSPI (CS=12, **SCLK=11** ← different!, SDIO0..3=4..7, RST routed via XCA9554 EXIO1)
- Touch: **FT3168** via I2C (SDA=15, SCL=14, INT=21, addr=0x38). Driven by minimal inline reader in `main.cpp` (FocalTech standard register layout — avoids vendoring the GPLv3 `Arduino_DriveBus` library).
- PMU: AXP2101 @ 0x34 (same chip as 2.16 — `XPowersLib` reused; battery is an optional kit add-on but PMU + charging circuitry are populated)
- IMU: QMI8658 @ 0x6B (same chip — initialized for I2C bus health, rotation logic disabled)
- IO expander: **XCA9554 / PCA9554** @ I2C 0x20. Gates LCD_RST, TP_RST, audio amp enable, and reads the PWR button. **`io_expander_init()` MUST run before `gfx->begin()` or `ft3168_init()`** — otherwise display/touch stay in reset and silently fail. PWR button is on EXIO4, active HIGH (verified empirically with the deleted `iox` serial debug command).
- Orientation: **fixed at 0°**. IMU auto-rotation is disabled; `rotate_strip()` / `handle_rotation_change()` are excluded via `#ifndef BOARD_AMOLED_18`.
- Buttons: GPIO 0 (BOOT → Space/voice-mode), XCA9554 EXIO4 (PWR → cycle screens; on splash → cycle animations). **No third button** (GPIO 18 button doesn't exist on this board).

### AMOLED-2.06 (watch form factor)
- Display: **CO5300** AMOLED via QSPI (CS=12, **SCLK=11** ← same as 1.8, SDIO0..3=4..7, RST=8 direct GPIO). 410×502 portrait. Requires **`col_offset1 = 23`** in the Arduino_CO5300 constructor — the panel's visible viewport sits at a 22–23 column offset inside the controller's internal RAM. Without it, a vertical strip of stale/garbage content shows through on the right edge (we picked 23 empirically; Waveshare's reference library uses 22).
- Touch: **FT3168** via I2C (SDA=15, SCL=14, **INT=38, RST=9** direct GPIO, addr=0x38). Same inline reader vendored from the 1.8 port (no GPLv3 dependency). Coordinates verified end-to-end with the BLE reset zone.
- PMU: AXP2101 @ 0x34 (same chip as 2.16/1.8 — `XPowersLib` reused). PWR button routes through AXP PKEY short-press IRQ (no IO expander).
- IMU: QMI8658 @ 0x6B (initialized for I2C bus health; rotation logic disabled — fixed watch enclosure orientation).
- RTC: **PCF85063** on the same I2C bus, powered through AXP2101 for retention. Not currently used by Clawdmeter but present for future features.
- Audio codec: **ES8311** + ES7210 ADC on the same I2C bus. Unused by Clawdmeter; just don't accidentally clash with their I2C addresses.
- **No IO expander** despite the Waveshare wiki FAQ implying one. The schematic shows Key3/PWR wires directly to AXP2101 PWRON; touch reset and display reset are direct GPIOs. `board_init()` pulses LCD_RESET (GPIO 8) and TP_RESET (GPIO 9) before display/touch HAL init.
- Buttons: GPIO 0 (BOOT → Space/voice-mode), AXP PKEY (PWR → cycle screens). **No third button**.
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
    waveshare_amoled_206/   — CO5300 + FT3168 + AXP PKEY, no IO expander, 32 MB, no rotation
    template/               — copy this to bootstrap a new port
  main.cpp                  — setup() + loop(): HAL calls only, zero #ifdef BOARD_*
  ui.{h,cpp}                — 3-screen UI (splash, usage, bluetooth). compute_layout() picks fonts/positions from board_caps() (responsive — current breakpoint: H >= 460 → large, else compact)
  splash.{h,cpp}            — 20×20 pixel-art engine. CELL = min(W,H)/20, centered.
  ble.{h,cpp}               — NimBLE peripheral: custom data service + HID keyboard
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
pio run -d firmware -e waveshare_amoled_216                                     # build 2.16 (default original)
pio run -d firmware -e waveshare_amoled_18                                      # build 1.8 (newer port)
pio run -d firmware -e waveshare_amoled_206                                     # build 2.06 (watch port)
pio run -d firmware -e waveshare_amoled_18 -t upload --upload-port /dev/cu.usbmodem101    # flash 1.8 on macOS
pio run -d firmware -e waveshare_amoled_216 -t upload --upload-port /dev/ttyACM0          # flash 2.16 on Linux
pio run -d firmware -e waveshare_amoled_206 -t upload --upload-port /dev/cu.usbmodem101   # flash 2.06 on macOS
```

If `pio` isn't on PATH: try `~/.platformio/penv/bin/pio` (Linux/macOS pio install) or `brew install platformio` on macOS.

Device path differs by OS: `/dev/cu.usbmodem*` on macOS, `/dev/ttyACM0` on Linux. Both expose the ESP32-S3 native USB-JTAG (no boot-mode dance needed).

## QA your own UI changes — don't ask the user

The firmware ships a `screenshot` serial command that dumps the LVGL framebuffer. `./screenshot.sh out.png [port]` captures a PNG sized to the active display (480×480 or 368×448). **Use this on every UI iteration** — Read the PNG with the Read tool, verify the change visually, iterate. Script auto-picks the macOS/Linux default port and falls back to pio's bundled Python if pyserial isn't on the system Python.

The boot screen is `SCREEN_SPLASH` and only advances on a physical button press, so a fresh flash will sit on the splash. To screenshot the screen you're actually editing without asking the user to press a button, **temporarily change the default boot screen** in `main.cpp` (search for `ui_show_screen(SCREEN_SPLASH);`) to `SCREEN_USAGE` / `SCREEN_CONTROLLER` / `SCREEN_BLUETOOTH`, do your iteration, then revert before committing.

## Critical gotchas

1. **CO5300 cannot rotate.** Its MADCTL only supports axis flips, not column/row exchange. Rotation is done by **CPU pixel remapping inside `display_hal_draw_bitmap`** in `boards/waveshare_amoled_216/display.cpp`. We use **PARTIAL render mode with strip rotation** (small 480×40 strips, fast). On rotation change → AMOLED brightness flash → force redraw (handled inside `display_hal_tick`).
2. **OPI PSRAM** required: `board_build.arduino.memory_type = qio_opi` in platformio.ini. Without this, `MALLOC_CAP_SPIRAM` returns NULL and the screen is black.
3. **pioarduino platform required.** GFX Library for Arduino needs Arduino Core 3.x (`esp32-hal-periman.h`), not the 2.x that standard `espressif32` ships. We pin `pioarduino/platform-espressif32` 55.03.38-1.
4. **LVGL 9 font patching.** `lv_font_conv` outputs LVGL 8 format. Must remove `#if LVGL_VERSION_MAJOR >= 8` guards, drop `.cache` field, add `.release_glyph`, `.kerning`, `.static_bitmap`, `.fallback`, `.user_data`. Without patching, fonts render invisible.
5. **Touch reading is centralized inside each board's `touch.cpp`.** The HAL `touch_hal_read()` is called once per loop from `my_touch_cb`; the board's implementation owns its latched `touch_pressed/x/y` state. Don't call the underlying controller from anywhere else — CST9220's `getPoint()` etc. do a full I2C transaction and concurrent callers consume each other's data.
6. **Even-aligned flush regions.** `display_hal_round_area` (called from `rounder_cb`) is what each board uses to enforce this. Required on CO5300, harmless on SH8601.
7. **Touch axis swap/mirror is per-board.** The 2.16's CST9220 needs `setSwapXY(true)` + `setMirrorXY(true, false)` — applied inside `boards/waveshare_amoled_216/touch.cpp::touch_hal_init()`. New ports apply their own.
8. **LVGL RGB565A8 is planar.** `w*h` RGB565 pixels followed by `w*h` alpha bytes; `data_size = w*h*3`, `stride = w*2`. Use `init_icon_dsc_rgb565a8()` for icons that overlap non-uniform backgrounds (e.g. battery over splash). Lucide source PNGs are black-on-transparent — converter must tint to white or icons render invisible. See `tools/png_to_lvgl.js`.
9. **Per-board pre-init is `board_init()`.** Each board's `board_init.cpp` brings up `Wire` and any reset-gating IO expander BEFORE `display_hal_init()`. Skipping the IO expander release on AMOLED-1.8 leaves SH8601 + FT3168 in reset and they silently fail to probe.
10. **No `#ifdef BOARD_*` in shared code.** The whole point of the refactor — if you're about to add one, you probably want a `BoardCaps` field or a per-board file instead. See `docs/porting/capability-flags.md`.

## Icons

`tools/png_to_lvgl.js <input.png> <symbol> [W_MACRO] [H_MACRO] [--tint=RRGGBB | --no-tint]` converts an alpha PNG to RGB565A8. Default tint is white (`0xFFFFFF`) — necessary for Lucide PNGs. Splice output into `firmware/src/icons.h` and use `init_icon_dsc_rgb565a8()` in ui.cpp. Currently only the 5 battery icons use this format; the rest are still raw RGB565 baked over the panel background, fine because they live inside opaque zones.

## Splash animations

13 × 20×20 pixel-art creature animations sourced from
[claudepix.vercel.app](https://claudepix.vercel.app). Pipeline:

```bash
node tools/scrape_claudepix.js  # → tools/claudepix_data/*.json
node tools/convert_to_c.js      # → firmware/src/splash_animations.h
```

Each animation has a per-animation 10-color RGB565 palette. Cell values 0..9 index it. Default boot screen.

## User profile / preferences

See `~/.claude/projects/.../memory/` files for persistent context (user is an embedded-beginner senior dev, brand-conscious, prefers iterative UI refinement, dislikes me authoring my own art when third-party assets are intended). Always read those memory files at session start.

## Recent session highlights

- **Device-abstraction refactor (2026-05-18).** All board-conditional code moved out of shared files into `boards/<name>/` and behind a HAL in `hal/`. ~30 `#ifdef BOARD_*` blocks went to zero. UI is responsive via `compute_layout()` driven by `board_caps()`. New ports add a folder + a PlatformIO env — no shared file edits.
- Added second board port: Waveshare AMOLED-1.8 (368×448 portrait, SH8601, FT3168, XCA9554 IO expander).
- Migrated from Panlee SC01 Plus (480×320 IPS) to Waveshare 2.16" AMOLED (480×480 square). Full hardware/library swap.
- Added IMU auto-rotation, battery indicator, USB-state-aware screen switching.
- Added splash screen with scraped pixel-art animations and 3-button physical input layout.
- Fonts and icons re-scaled ~1.9× for the higher-DPI panel.
- All UI margins widened to 20px to clear the rounded display corners.
- Battery icons converted to RGB565A8 alpha so they blend cleanly over the splash animations.

## Daemon / host side

Bash daemon (`daemon/claude-usage-daemon.sh`) reads OAuth token, polls Anthropic API, sends JSON over BLE GATT. Run with `systemctl --user start claude-usage-daemon`. The unit file's `ExecStart` is the absolute path to the script — repoint it when switching between the worktree and the main checkout.

**Discovery & resilience:**

- Connects by name (`"Claude Controller"`) on first run, caches resolved MAC at `~/.config/claude-usage-monitor/ble-address`. ESP32 BLE addresses are factory-burned per-chip, so swapping any board invalidates the cache.
- On connect failure: cache is dropped AND device is removed from bluez (`bluetoothctl remove`) so the next scan won't re-pick a dead MAC. Multi-candidate scans pick `head -1` and let the failure cycle converge.
- `POLL_INTERVAL=60`, `TICK=5`. Inner loop wakes every 5s to detect disconnects fast; polls Anthropic when 60s elapsed OR when ESP fires a refresh request.

**GATT characteristics on service `4c41555a-...0001`:**

- `...0002` RX — daemon writes JSON usage payload here.
- `...0003` TX — firmware notifies ack/nack (daemon doesn't subscribe).
- `...0004` REQ — firmware fires `0x01` notify in `onSubscribe` if `has_received_data` is false. Daemon subscribes via `setsid bash -c "stdbuf -oL dbus-monitor … | awk …"`; awk drops a flag file the inner loop picks up. See the `feedback_dbus_monitor_pipe` memory for the three subtle gotchas (pipe buffering, busctl-exits race, `wait` blocking on pipeline jobs).
