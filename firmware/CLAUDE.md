# Firmware

PlatformIO project for the device; build and test commands are in the root
`CLAUDE.md`. Every board folder `src/boards/<env>/` has a `CLAUDE.md` with its
pins and hardware gotchas, which loads when you work in that folder.

## Structure

- **Shared code** (`src/*.cpp`) is board-independent. It calls the HAL in
  `src/hal/` (display, touch, input, power, IMU, sound, board info) and reads
  `BoardCaps` (runtime) and `BOARD_HAS_*` (compile time). Contract:
  `docs/porting/hal-contract.md`; flags: `docs/porting/capability-flags.md`.
- **Board folders** hold `board.h` (pins, I2C addresses, `BOARD_HAS_*`),
  `board_init.cpp` (Wire, IO expanders, power rails), the HAL sources,
  `caps.cpp` (the `BoardCaps` instance), private drivers and `CLAUDE.md`.
  Each env's `build_src_filter` compiles the shared code plus exactly one
  board folder. Start new ports from `boards/template/`. `boards/sim/` is the
  SDL2 simulator (HAL against SDL2, Arduino shims in `shim/`, BLE playback
  from `firmware/sim/*.jsonl`).
- **Data path** (`main.cpp`): BLE payload → `payload.cpp` (JSON → `UsageData`)
  → `signals.cpp` (fast clock: activity, events, sessions; feeds
  `usage_rate.cpp`, the slow clock that turns a 15-min window of real samples
  into Idle/Normal/Active/Heavy) → the director. Buttons and serial commands
  are in `main.cpp` too.
- **Clawd**: `clawd_director.cpp` owns the rooms and runs the beats;
  `clawd_policy.cpp` holds the pure rules and every timing (`ClawdTuning`,
  `clawd_tuning_fast()` under `SIM_FAST`). There is no separate stage:
  `performer.cpp` plays the full-size scenes on the meters, in front of the
  panels (PSRAM boards; C6 plays corner acts instead). Liveliness (`lv`,
  1 No-nonsense … 5 All-nonsense, kept in NVS by `clawd_prefs.cpp`) acts
  only through `clawd_tuning_scaled()`; add new behaviour to the tuning
  rather than checking the level elsewhere. The module map, every beat, the
  tunables and the C6 differences are in `docs/clawd-director.md`: read it
  before changing what Clawd does, and update it with the change.
- **Meters**: `ui.cpp` draws only the meters, where `layout.cpp` says.
  `layout_compute()` derives every position and font from `BoardCaps`'
  width, height and `round_panel`: the largest of three tuned designs that
  fits, then shape rules (narrow, tall, short, round). Strings it sizes for
  are in `ui_text.h`. Never branch on a board or exact size; add a shape rule
  and a `test_layout` panel (`docs/porting/adding-a-board.md`).
- **Sound**: `ding.cpp` synthesizes the one sound (a C#5 ding, mirrored by
  the desktop's `chime.rs`); `chime.cpp` streams it through the ES8311.
- **Generated, never hand-edited**: `splash_animations.h` (only
  `clawd_anim.cpp` includes it), `font_*.c`. Regenerate with the
  `regenerating-firmware-assets` skill.
- **Tests**: `firmware/test/test_*`, native, run with
  `pio test -d firmware -e native`. Keep pure logic (parsing, signals,
  policy, geometry, synthesis) out of LVGL/Arduino code so it stays testable
  there.

**C6 boards have no PSRAM.** Shared code gates on `BOARD_HAS_PSRAM` (absent on
C6) and allocates LVGL and animation buffers with `MALLOC_CAP_INTERNAL`. The
`screenshot` serial command is compiled out (`LV_USE_SNAPSHOT=0`), so C6 UI
changes must be eyeballed on hardware.

UI changes: check them yourself with the `verifying-firmware-ui` skill. The
simulator grid (`tools/simgrid/`, `docs/sim-grid.md`) shows every panel at
once from one script; the sim's controller mode behind it is
`src/boards/sim/sim_ctrl.cpp`.

## Critical gotchas

1. **No `#ifdef BOARD_*` in shared code.** Add a `BoardCaps` field, a HAL
   function or a per-board file instead (`docs/porting/capability-flags.md`).
2. **One Clawd on screen, ever.** Corner sprite, full-size sprite, performer
   and face are never visible together; beats wait for `mascot_away()` /
   `performer_visible()` / the face's drop before the next appears.
   `test/test_director` fuzzes the real director against fake actors and
   fails on any tick with two. Don't add a second creature (stale data dims
   the meters instead).
3. **"No data" is not a 0 % sample.** `signals` feeds only `ok && !rp`
   payloads to the rate ring and the reset detector; anything else makes the
   reset chime fire falsely.
4. **`board_init()` runs before `display_hal_init()`.** It brings up `Wire`
   and releases any IO expander or power rail gating the panel and touch.
   Skip it and they silently fail to probe (AMOLED-1.8, LCD-4, C6 1.8).
5. **Touch is read in one place**: each board's `touch.cpp`, through
   `touch_hal_read()`, called once per loop from `my_touch_cb`; the board owns
   the latched `touch_pressed/x/y`. Never read the controller elsewhere: a
   read is a full I2C transaction and concurrent readers consume each
   other's events.
6. **Touch axis swap/mirror is per board**, applied in that board's
   `touch_hal_init()`.
7. **Flush regions are even-aligned** by each board's `display_hal_round_area`
   (called from `rounder_cb`). Required on CO5300, harmless on SH8601.
8. **CO5300 cannot rotate in hardware.** Its MADCTL only flips axes; it can't
   exchange rows and columns. The AMOLED-2.16 rotates by CPU pixel remapping
   in `display_hal_draw_bitmap` (PARTIAL render mode, 480×40 strips) and
   forces a redraw on a rotation change (`display_hal_tick`).
9. **S3 needs OPI PSRAM**: `board_build.arduino.memory_type = qio_opi`.
   Without it `MALLOC_CAP_SPIRAM` returns NULL and the screen stays black.
10. **pioarduino platform, pinned** (`pioarduino/platform-espressif32`
    55.03.38-1): GFX Library for Arduino needs Arduino Core 3.x
    (`esp32-hal-periman.h`), which stock `espressif32` doesn't ship.
11. **Fonts need an LVGL 9 patch** after `lv_font_conv`, or they compile and
    render invisible (`regenerating-firmware-assets` skill).
12. **LVGL RGB565A8 is planar**: `w*h` RGB565 pixels, then `w*h` alpha bytes
    (`data_size = w*h*3`, `stride = w*2`). Icons over non-uniform
    backgrounds use it via `init_icon_dsc_rgb565a8()`; Lucide PNGs must be
    tinted white or they render invisible.
13. **RGB parallel panels need SRAM bounce buffers** (LCD-4):
    `Arduino_RGB_Display` DMA-scans PSRAM, so pass
    `bounce_buffer_size_px = LCD_WIDTH * 10`, and never call
    `getFrameBuffer()` after `gfx->begin()` (it builds a second panel and
    crashes).
