# HAL contract

Each board implements the interfaces in `firmware/src/hal/`. Shared code
uses these interfaces for hardware access. Initialization order, bounded
I2C work, and edge-triggered input are part of the contract.

## `board_caps.h`

Runtime description of the board. Provided by your `caps.cpp` as a
single `const BoardCaps` instance returned from `board_caps()`. The UI
queries this at startup and gates optional features (battery indicator,
secondary-button HID mapping) by what's true here. Keep the struct in
sync with the compile-time `BOARD_HAS_*` flags in `board.h`.

## `display_hal.h`

| Function                    | Responsibility |
|-----------------------------|----------------|
| `display_hal_init`          | Construct the display bus and driver (QSPI, SPI, or RGB). Must run AFTER `board_init()` so any IO expander has released the LCD reset line. |
| `display_hal_begin`         | `gfx->begin()`, clear screen, set default brightness. Allocate any rotation buffers needed by `display_hal_draw_bitmap`. |
| `display_hal_set_brightness`| Pass-through to the driver. Driver-defined scale (typically 0..255). |
| `display_hal_draw_bitmap`   | Push a w×h RGB565 strip at (x, y). If the panel can't rotate natively, apply CPU rotation here before pushing — `imu_hal_rotation_quadrant()` returns the current orientation. **Must complete inside LVGL's render budget** (a few ms at typical strip sizes). |
| `display_hal_tick`          | Per-loop housekeeping — used by rotation-aware boards to blank the panel + ramp brightness during a rotation transition. No-op on boards without rotation. |
| `display_hal_round_area`    | LVGL invalidate-area hook. Most QSPI AMOLED drivers expect even-aligned flush regions; apply `& ~1` / `| 1` to coordinates. |

## `touch_hal.h`

| Function          | Responsibility |
|-------------------|----------------|
| `touch_hal_init`  | Initialize the controller and configure interrupts or polling. Configure axis swap / mirror so coordinates returned in `touch_hal_read` match the panel's pixel coordinates after any rotation. |
| `touch_hal_read`  | Return the latest sample. **Hard requirement: complete in well under 5 ms** — LVGL polls this every screen refresh and any I2C burst longer than a screen tick will visibly stutter. |

Follow the project dependency policy when choosing a driver. The
AMOLED-1.8 port includes a small I2C reader as an implementation reference.

## `input_hal.h`

| Function          | Responsibility |
|-------------------|----------------|
| `input_hal_init`  | `pinMode()` for the physical button GPIOs. |
| `input_hal_is_held` | Return true while the button is held. Active-low pull-up GPIOs are typical. Boards lacking a secondary button must return `false` for `INPUT_BTN_SECONDARY`. |

PWR belongs to `power_hal`, whether its input comes from a PMU, IO
expander, or GPIO. The LCD-4 hardware reset key is not a software PWR input.

## `power_hal.h`

| Function                | Responsibility |
|-------------------------|----------------|
| `power_hal_init`        | Bring up the PMU (if any). Configure battery measurement. Subscribe to the PWR button source (PMU IRQ or IO expander polling). |
| `power_hal_tick`        | Refresh battery % and charging state at sensible intervals (the reference ports use 2s / 500 ms). Poll the PWR button if it's not interrupt-driven. |
| `power_hal_battery_pct` | 0..100, or `-1` when battery info isn't available. |
| `power_hal_is_charging` | Bool, false on no-battery boards. |
| `power_hal_pwr_pressed` | **Edge-triggered**: returns true once per short-press, then clears. Shared code calls this every loop and expects one true per press. |

Absent features need no-op implementations. Return `-1` for unavailable
battery data and `false` for absent button events. Set `has_battery` false
to hide the battery indicator. USB power detection is independent of
battery presence.

Also implement `power_hal_is_vbus_in()` for USB power status,
`power_hal_pwr_long_pressed()` for a one-shot long-press event, and
`power_hal_pwr_released()` for a one-shot release event. The pairing gesture
uses the long-press and release edges; a held level is not a substitute.

## `sound_hal.h`

Implement `sound_hal_init()`, `sound_hal_tick()`, `sound_hal_chime()`,
and `sound_hal_tune(gain, volume)`. Playback must return promptly and
advance during ticks. `sound_hal_tune` backs the `chime <gain> [volume]`
serial command, which sets the level until reboot. Use it to tune a new
board's speaker by ear, then set that board's defaults in its `sound.cpp`. Boards without an implemented audio output use no-ops. A fitted
codec alone does not mean the firmware supports sound.

## `imu_hal.h`

| Function                     | Responsibility |
|------------------------------|----------------|
| `imu_hal_init`               | Bring up the accelerometer. |
| `imu_hal_tick`               | Sample the accelerometer at a low rate (~10 Hz) and update the rotation state with hysteresis. |
| `imu_hal_rotation_quadrant`  | Current rotation, 0..3 (quarter turns CW). Used by `display_hal_draw_bitmap` on rotation-capable boards. Boards without rotation always return 0. |

## `board_info_hal.h` (optional)

Weak defaults live in `firmware/src/board_info_default.cpp`; implement a
function in your board folder only when it has something to report.

| Function         | Responsibility |
|------------------|----------------|
| `board_hal_rev`  | Short, stable hardware-revision token for the `info` serial command (`{"env","ver","rev","link"}`), which the desktop flasher uses to identify a board before re-flashing. Default `""`. Valid after `board_init()`; return a static string. AMOLED-1.8 returns `sh8601_ft3168` or `co5300_cst816` from its boot-time probe. |

## Serial console commands

`check_serial_cmd()` in `main.cpp` (115200 baud, newline-terminated):

| Command                   | Reply |
|---------------------------|-------|
| `screenshot`              | `SCREENSHOT_START w h bytes`, raw RGB565, `SCREENSHOT_END` (`SCREENSHOT_UNSUPPORTED` without PSRAM) |
| `chime` (or `buzz`)       | plays the ding (no reply) |
| `chime <gain> [volume]`   | tunes a board's speaker until reboot (gain 0..1, ES8311 volume 0..100) and plays the ding. Replies `{"chime":{"gain":0.70,"volume":77}}`. |
| `unpair`                  | clears every BLE bond and re-advertises. Replies `{"unpair":true}`. |
| `ble`                     | `BLE status:` lines: state, bond count, owner, then each connected peer's address, identity address, encryption and bond flags. |
| `disconnect`              | drops every BLE connection (no reply); the board re-advertises. |
| `perf`                    | prints the redraw cost (refreshes, time, pixels sent, longest gap) of the last face rise, settle, drop and performance. |
| `info`                    | one JSON line: `{"env":"waveshare_amoled_18","ver":"v1.9.0","rev":"co5300_cst816","link":"ble"}`. `env` comes from `-DCLAWD_ENV` (set per env in `platformio.ini` from `${this.__env__}`), `ver` from `-DCLAWD_VERSION` (defaults to `dev`; CI sets `PLATFORMIO_BUILD_FLAGS='-DCLAWD_VERSION=\"vX.Y.Z\"'`), `rev` from `board_hal_rev()`. |
| `clawd <beat>`            | starts a Clawd beat now, for `screenshot` QA without waiting for the situation: `show`, `screensaver`, `attention`, `joy`, `lurk` (a lurk trip, PSRAM boards), `peek [x]` (curious face), `home` (return home). Replies `{"clawd":"show","ok":true}`. See `docs/clawd-director.md`. |

## Layout and visual checks

The meters layout comes from `BoardCaps.width`, `height` and `round_panel`
(`layout.cpp`, described in [Add a board](adding-a-board.md#the-meters-layout)).
Add the panel's size to `test_layout` to check that every label fits.

Clawd's full-size performances use a 60×60-cell coordinate system, with cell size derived from
`min(width, height) / 60`. Corner Clawd has a separate seat and scale.
Check meters, spend-only accounts, pairing, stale data, attention, the
face, and minimal display on the new panel. Use `perf` to inspect redraw
costs and [the simulator guide](../simulator.md) for scripted captures.
