# Project context

ESP32 firmware for a desk-side Claude Code usage monitor on an **ideaspark ESP32 1.14" ST7789** board (135×240 IPS SPI LCD). Connects to a host daemon over BLE; daemon polls Anthropic API for usage data.

This file is for future Claude Code sessions to bootstrap quickly. Read this first.

## Hardware (critical pins)

- MCU: **ESP32-WROOM-32** (240 MHz, 320 KB SRAM, 4 MB Flash, **no PSRAM**, BLE 4.2)
- Display: **ST7789** 135×240 IPS via SPI — MOSI=23, SCLK=18, CS=15, DC=2, RST=4, BLK=32
- Touch: **none**
- PMU: **none** (no battery, no AXP)
- IMU: **none** (no auto-rotation; rotation fixed at 0)
- USB-serial: CH340 on **COM13** (Windows)
- Buttons: GPIO 0 (back/left button only). GPIO 18 is LCD SCLK — **cannot be used as button**.
  Short press = cycle screen. Long press (≥700 ms) = context action: on Bluetooth, clear BLE bonds; on Clock, toggle the focus timer; elsewhere, request a fresh daemon poll.

## Architecture

```text
main.cpp        — setup(), loop(), BTN_BACK (GPIO0) short/long-press polling, serial commands (screenshot, etc.)
display_cfg.h   — pin defines (ST7789 SPI), LCD_WIDTH=135, LCD_HEIGHT=240, extern bus/gfx objects
ui.{h,cpp}      — 11-screen UI (see Screens below); redesigned for 135×240
splash.{h,cpp}  — 20×20 pixel-art animation engine; 6× upscale (120×120) on splash, 4× (80×80) on Copilot screen
env_sensor.{h,cpp} — BME280/BMP180 dispatcher; owns Wire.begin(21, 22)
sensor_hist.{h,cpp} — 24 h ring buffer of sensor readings (96 × 15 min), persisted to NVS
usage_hist.{h,cpp}  — 24 h ring buffer of Claude session/weekly usage % (same
                       design as sensor_hist, kept as a sibling module — see
                       usage_hist.h for why), feeds the Usage screen sparklines
imu.{h,cpp}     — stub: imu_get_rotation() always returns 0
power.{h,cpp}   — stub: battery=-1, charging=false, pwr_pressed=false
ble.{h,cpp}     — NimBLE peripheral: custom data service + HID keyboard (unchanged)
data.h          — UsageData, CopilotData, SysInfoData, VscodeData, CiData, EnvData structs (one per BLE payload "src")
icons.h         — icon arrays (RGB565)
logo.h          — 80×80 RGB565 logo (hidden at runtime — too wide for 135px)
font_*.c        — LVGL 9 bitmap fonts in use: Tiempos 34, Styrene 24/16/14/12, Mono 18
splash_animations.h — generated, do not hand-edit
```

## Screens

`screen_t` in `ui.h` (SCREEN_COUNT = 11). Short-press cycles in this order, skipping any screen that has never received data from the daemon: Clock → Sensor → Sensor Trend → Usage → Copilot → SysInfo → VS Code → Bluetooth → CI (labeled "Checks" in UI) → Today → Splash → Clock…

| Screen | Payload `src` | Shows |
| --- | --- | --- |
| `SCREEN_SPLASH` | — | Boot animation; default screen, only advances on button press |
| `SCREEN_CLOCK` | `env` | Time/date, weather (temp, hi/lo, condition, location), a Claude+Copilot usage strip; long-press toggles the focus timer |
| `SCREEN_SENSOR` | on-device I2C | BME280/BMP180 temp/humidity/pressure + sparkline; skipped if no sensor wired |
| `SCREEN_SENSOR_GRAPH` | on-device I2C | 24 h trend panels (see `sensor_hist.{h,cpp}` below) |
| `SCREEN_USAGE` | `claude` (default `src`) | Session (5 h) and weekly (7 d) % bars, reset countdowns, active model + activity status word |
| `SCREEN_COPILOT` | `copilot` | Premium-request % used, remaining/total, reset date, plan name |
| `SCREEN_SYSINFO` | `sysinfo` | Host CPU/RAM/disk % and temp |
| `SCREEN_VSCODE` | `vscode` | VS Code process RSS, CPU %, extension host count, recent error |
| `SCREEN_BLUETOOTH` | — | BLE state, device name/MAC, hold-to-unpair hint |
| `SCREEN_CI` | `ci` | CI pass/fail state + age, PRs awaiting review, dirty/ahead/behind git status |
| `SCREEN_TODAY` | `sum` | Daily digest: active minutes, tokens, cost, commits, Copilot requests used |

`act` payloads (`ui_update_act`) don't own a screen — they drive the activity dot/word on Usage and, on `ACT_NEEDS_INPUT`, a persistent "Claude needs you" banner that overlays whatever screen is active until dismissed by a press. The focus timer (Clock long-press) uses the same banner widget for a transient "Focus"/"Break time" message.

## Build / flash (Windows)

```powershell
# Build
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -d firmware

# Flash
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -d firmware -t upload --upload-port COM13
```

Device is on COM13 via CH340 USB-serial. Hold BOOT (GPIO0) while pressing EN if the chip doesn't enter download mode automatically (rarely needed).

## QA your own UI changes — don't ask the user

The firmware ships a `screenshot` serial command that dumps the LVGL framebuffer over the serial port. `./screenshot.sh out.png COM13` (or the Windows equivalent) captures a 135×240 PNG. **Use this on every UI iteration** — read the PNG, verify visually, iterate.

The boot screen is `SCREEN_SPLASH` and only advances on a physical button press. To screenshot a different screen without asking the user to press a button, **temporarily change the default boot screen** in `main.cpp` (search for `ui_show_screen(SCREEN_SPLASH);`) to any value from the `screen_t` enum (see Screens above), iterate, then revert before committing. Alternatively, use the serial QA commands without touching code: `screen <N>` jumps to a screen index directly, and `feed <json>` injects a payload (e.g. `feed {"src":"ci","state":"fail",...}`) as if it arrived over BLE — see `check_serial_cmd()` in `main.cpp` for the full command list (`timer`, `histfill`, `histclear`, `uhistfill`, `uhistclear`, `i2cscan`, `gpiotest`).

## Critical gotchas

1. **pioarduino platform required.** GFX Library for Arduino 1.6.x needs Arduino Core 3.x (`esp32-hal-periman.h`), which standard `espressif32` 7.x does NOT provide. We use `pioarduino/platform-espressif32` (stable zip URL in platformio.ini). Standard `espressif32` gives Core 2.x → compile failure.
2. **No PSRAM.** ESP32-WROOM-32 has no SPIRAM. All `heap_caps_malloc(…MALLOC_CAP_SPIRAM)` must be plain `malloc()`. LVGL partial render buffers are 135×40×2 = 10,800 bytes each — fits comfortably in SRAM.
3. **ST7789 col/row offsets.** The 1.14" 135×240 panel needs `col_offset=52, row_offset=40` in `Arduino_ST7789` constructor or the image is shifted off-screen.
4. **GPIO 18 = LCD SCLK.** Cannot be used for any other purpose (was BTN_FWD on the old board). BTN_FWD/right-button is permanently disabled.
5. **Check flash before adding large assets.** The partition scheme gives ~1.87 MB app space (`Flash: [======    ]  62.9%` as of 2026-09-11 — this number drifts with every feature added, so treat it as "check `pio run`'s own report," not as a fact to trust from this doc). Don't add large new font files or feature libraries without checking size first.
6. **NimBLE-Arduino 2.5.0 deprecation warning.** `svc->start()` in `ble.cpp:154` emits a `-Wdeprecated-declarations` warning. This is harmless — the call is a no-op that still compiles and links cleanly. Do not remove it without testing BLE.
7. **LVGL 9 font patching.** `lv_font_conv` outputs LVGL 8 format. Must remove `#if LVGL_VERSION_MAJOR >= 8` guards, drop `.cache` field, add `.release_glyph`, `.kerning`, `.static_bitmap`, `.fallback`, `.user_data`. Without patching, fonts render invisible.
8. **LVGL RGB565A8 is planar.** `w*h` RGB565 pixels followed by `w*h` alpha bytes; `data_size = w*h*3`, `stride = w*2`. Use `init_icon_dsc_rgb565a8()` for icons that overlap non-uniform backgrounds. Lucide source PNGs are black-on-transparent — converter must tint to white. See `tools/png_to_lvgl.js`.

## Icons

`tools/png_to_lvgl.js <input.png> <symbol> [W_MACRO] [H_MACRO] [--tint=RRGGBB | --no-tint]` converts an alpha PNG to RGB565A8. Default tint is white (`0xFFFFFF`) — necessary for Lucide PNGs. Splice output into `firmware/src/icons.h` and use `init_icon_dsc_rgb565a8()` in ui.cpp.

## Splash animations

15 × 20×20 pixel-art animations. 13 sourced from [claudepix.vercel.app](https://claudepix.vercel.app) (splash screen). 2 are manually authored Copilot mascot animations (`copilot_idle.json`, `copilot_thinking.json`). Pipeline:

```bash
node tools/scrape_claudepix.js  # → tools/claudepix_data/*.json (claudepix only)
node tools/convert_to_c.js      # → firmware/src/splash_animations.h (all 15)
```

Each animation has a per-animation palette (up to 10 colors). Cell values index it. Splash screen renders at 6× scale (120×120). Copilot screen renders at 4× scale (80×80) and cycles between `copilot_idle` (10s) and `copilot_thinking` (5s). Do not hand-edit `splash_animations.h` — regenerate with `convert_to_c.js`.

## Recent session highlights

- Originally targeted Waveshare ESP32-S3-Touch-AMOLED-2.16 (480×480, CO5300, PSRAM, IMU, touch, PMU). See git history for that version.
- **Ported to ideaspark ESP32 1.14" ST7789** (135×240, no PSRAM, no IMU, no touch, no PMU). Full hardware swap: SPI display driver, stub power/IMU, complete UI layout redesign for 135×240, LVGL buffers in SRAM.
- pioarduino platform retained (needed by GFX Library 1.6.x for `esp32-hal-periman.h`); switched board from `esp32s3box` → `esp32dev`.
- Flash usage was tight (~90%) right after the port; has since eased as the partition scheme/build settled (~63% as of 2026-09-11) — see gotcha #5, don't trust either number without checking `pio run`'s own report.
- **Added Copilot screen** (`SCREEN_COPILOT`) — VS Code-style premium-request usage panel: big colored `%` label (`lbl_copilot_pct`, `font_styrene_24`, colored by `pct_color()`), fraction right-aligned, progress bar (green/amber/red), reset date. Panel height 90px.
- **Copilot animations**: `COPILOT_CELL=4` → 80×80 canvas. Cycles idle↔thinking: `COPILOT_IDLE_HOLD_MS=10000`, `COPILOT_THINK_HOLD_MS=5000`. Both animation indices looked up by name in `splash_copilot_init()`.
- **`copilot_thinking.json`** manually authored: 4 frames, squinting eyes + light-blue thought dots (upper-right). Not from claudepix scraper — keep in `tools/claudepix_data/` alongside `copilot_idle.json`.
- **`data.h` `CopilotData`**: fields `premium_pct`, `premium_remaining`, `premium_total`, `premium_reset_mins`, `premium_reset_str[24]`, `plan[20]`, `enabled`, `valid`.
- **`daemon/claude_usage_daemon.py`**: plan name mapping (`individual`→`Pro` etc.), `prd` formatted reset date field.
- **Sensor trend graphs** (`sensor_hist.{h,cpp}` + the `SCREEN_SENSOR_GRAPH` page in `ui.cpp`): 24 h of temp/humidity/pressure in a 96-slot × 15-minute ring, mean-averaged per slot, flushed to NVS (namespace `shist`, key `ring`, 588-byte blob) on every slot roll so the trend survives a reboot. The Sensor page carries a temperature sparkline; the "Trend 24h" page stacks one auto-scaled panel per metric. Traces are `lv_line` polylines, not `lv_chart` — `LV_USE_CHART` is not in `platformio.ini` and the polyline costs no extra flash.
- **No RTC on this board**, so the ring is indexed by absolute 15-minute buckets derived from the daemon's `{"src":"env"}` epoch. Until that arrives it free-runs on `millis()`; `sensor_hist_set_time()` re-anchors the ring and rolls it forward over the power-off gap. Serial QA commands: `histfill` (synthetic 24 h trace) and `histclear`.
- **Added Clock, SysInfo, VS Code, CI, and Today screens** (`SCREEN_CLOCK`/`SCREEN_SYSINFO`/`SCREEN_VSCODE`/`SCREEN_CI`/`SCREEN_TODAY`) — see the Screens table above for what each shows and its payload `src`. Clock is the effective "home" screen (first stop after Splash) and doubles as a focus-timer display.
- **"Claude needs you" banner** (`banner`/`lbl_banner` in `ui.cpp`) — a full-width overlay shown on `ACT_NEEDS_INPUT` (via `ui_update_act`), persistent until dismissed by any button press (`ui_banner_visible()`/`ui_hide_banner()` in `main.cpp`'s button handler intercept the dismiss-press so it doesn't also cycle the screen). The focus timer reuses the same widget for a transient 4 s "Focus"/"Break time" message (`banner_auto_hide_ms`).
- **Focus timer**: long-press on the Clock screen toggles it (`ui_timer_toggle()`, serial command `timer`). State machine is `TMR_FOCUS`/`TMR_BREAK`/off; entering a phase flashes the banner.
- **Navigation/alerting/UI pass** (2026-09-11 session — analyze-and-improve pass across navigation, alerting, visual polish, and usage-history trends):
  - **Screen-position dots** — a row of small dots near the top of the screen, shown for 1.5 s after every `ui_show_screen()` then auto-hidden (`ui.cpp`: `dot_objs`/`refresh_screen_dots()`/`dots_hide_ms`, ticked in `ui_tick_anim()`). Position is computed against `CYCLE_ORDER[]`, which mirrors `ui_cycle_screen()`'s traversal order — keep the two in sync if the cycle order ever changes. `screen_is_populated()` is the single shared predicate for "has this screen ever received data" (used by both the dots and the cycle-skip logic — don't reintroduce a second copy).
  - **Banner priority system** — the single shared "Claude needs you" banner widget now has a flat priority ranking (`banner_kind_t`: TIMER < ALERT < NEEDS_YOU) via `banner_show()`/`banner_clear()` in `ui.cpp`. It's a ranking, not a stack: a pre-empted lower-priority banner is dropped, not restored, when the higher one clears — see the comment above `banner_kind_t` before changing this. `BANNER_ALERT` is shared by CI-failure (`ui_update_ci()`) and the session/weekly ≥90% usage alert (`ui_update()`, `USAGE_ALERT_PCT`), each with its own one-shot "armed" flag so a payload doesn't re-fire the banner every poll.
  - **Double-press "home" gesture** (`main.cpp` button handler) — a second short-release within `DOUBLE_PRESS_MS` (350 ms) of the last one jumps straight to `SCREEN_CLOCK` instead of cycling one more step. Deliberately not a "wait to see if a second press follows" implementation (that would make every single press feel laggy with up to 10 screens in the cycle) — the first press always cycles immediately; a fast second one just redirects.
  - **Agent-count badge** — `g_act_agents` (from `{"src":"act"}`'s `n` field) was tracked but never rendered before this; now shown as "xN" left of the activity dot when N > 1 (`lbl_agent_badge` in `refresh_status_label()`).
  - **Checks-screen git-status chips** — replaced one dense "main 4 chg +1 -0" text line with a small branch marker + name, then color-coded pill chips (dirty=amber, ahead=green, behind=dim) in fixed slots that just leave a gap when hidden (`pill_ci_dirty`/`pill_ci_ahead`/`pill_ci_behind` in `ui_update_ci()`).
  - **Usage-screen sparklines** — 24 h session/weekly % history as micro `lv_line` traces (no axes/labels — genuinely a "sparkline") tucked into the free space right of each panel's reset-time label. Backed by `usage_hist.{h,cpp}`, fed every `ui_update()` call (`usage_spark_rebuild()`), sampled in `main.cpp`'s "claude" payload branch.
  - Fixed `lbl_anim`'s (Usage screen gerund spinner) text overflow: the longest words ("Philosophising…", "Flibbertigibbeting…") used to run off the 135px width uncropped — now `LV_LABEL_LONG_DOT` truncates with "...".

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

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

Rules:
- For codebase questions, first run `graphify query "<question>"` when graphify-out/graph.json exists. Use `graphify path "<A>" "<B>"` for relationships and `graphify explain "<concept>"` for focused concepts. These return a scoped subgraph, usually much smaller than GRAPH_REPORT.md or raw grep output.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).
