# Capability flags

Board capabilities keep hardware decisions out of shared UI code. Each
board declares compile-time facts in `board.h` and exposes runtime facts
through `caps.cpp`:

1. **`caps.cpp`**: copies them into the `BoardCaps` instance so shared
   code (`ui.cpp`, `main.cpp`) can query them at runtime via
   `board_caps()`.
2. **Per-board sources**: `#if BOARD_HAS_*` selects the implementation
   appropriate to that board.

Keep runtime fields consistent with board flags. For example:

```c
.button_count = (uint8_t)(1 + BOARD_HAS_SECONDARY_BUTTON),
.has_rotation = (bool)BOARD_HAS_ROTATION,
```

## Board flags

| Macro                          | Conventional value when absent | Purpose |
|--------------------------------|---------|---------------|
| `BOARD_HAS_SECONDARY_BUTTON`   | 0       | A second physical button (HID Shift+Tab on the reference ports). `caps.button_count = 1 + this`. UI uses `caps.button_count >= 2` to decide whether to poll/handle the secondary button — there is no `#ifdef` in shared code. |
| `BOARD_HAS_ROTATION`           | 0       | IMU-driven auto-rotation, implemented by the board display driver. The S3 2.16 rotates strips on the CPU; other drivers can use controller support. When 0, `display_hal_tick` is a no-op and the rotation buffer in `display.cpp` doesn't get allocated. |
| `BOARD_HAS_IMU`                | 0       | Whether the accelerometer is populated and initialized. Distinct from `BOARD_HAS_ROTATION` — the AMOLED-1.8 has the QMI8658 (so `BOARD_HAS_IMU=1`) but the kit's enclosure mounts the panel at a fixed orientation, so rotation is off. |
| `BOARD_HAS_BATTERY`            | 0       | Whether battery measurement is available on this board (PMU or ADC). UI hides the battery indicator when false. |
| `BOARD_HAS_IO_EXPANDER`        | 0       | Whether an IO expander gates display / touch reset lines. Doesn't directly gate any code path — but signals to the porter that `board_init()` must release the expander before `display_hal_init()`. |

## Runtime-only capabilities

Some `BoardCaps` fields have no `BOARD_HAS_*` macro because no per-board code
is compiled differently for them; set them directly in `caps.cpp`.

| Field          | Conventional value when absent | Purpose |
|----------------|---------|---------------|
| `minimal_ui` | `false` | Board-only minimal display: meters and a still corner Clawd, without stage, face, streak, or lurk trips. Set `true` for e-ink or slow panels. `minimal_mode()` returns `board_caps().minimal_ui`; there is no NVS override, desktop setting, or serial toggle. Attention uses a held pointing frame and a non-scrolling message; a tap or the 2-minute device timeout dismisses it and notifies the app. `SIM_MINIMAL=1` sets the simulator capability. |
| `has_pwr_button` | `false` | A PWR-role button reaches the firmware (AXP PKEY, IO-expander pin or GPIO): brightness, next scene on the stage, hold ~3 s and release to pair. `false` when there is none (LCD-4, whose KEY/PWR is a hardware reset): the pairing hint then says to pair from the computer, and a 5 s touch hold on it clears the bonds (`pair_hold.h`). The `unpair` serial command clears them on every board. |

## Build-flag macros

`BOARD_HAS_PSRAM` is set as a `-D` build flag in `platformio.ini` (not in `board.h`) on chips with external PSRAM wired up. Shared code (`main.cpp`, `splash.cpp`) and per-board display drivers use it to choose between `MALLOC_CAP_SPIRAM` (large buffers) and `MALLOC_CAP_INTERNAL` (smaller buffers, partial-render LVGL, direct-to-panel stage rendering, screenshot capture disabled). New ESP32-C6 / ESP32-C3 ports must leave this undefined.

## Adding capabilities

Add a new flag when:

- A shared-code decision currently uses `if (caps.has_<thing>)` and
  you want to extend it (e.g. add `BOARD_HAS_HAPTIC` for vibration
  feedback).
- A per-board file conditionally compiles a block of code (e.g.
  audio amp init under `BOARD_HAS_AUDIO`).

Don't add a flag for a one-off detail. If only one board cares about it
and shared code never queries it, leave it as a constant in that board's
`board.h` and use it only in that board's `.cpp` files.

`button_count` counts primary and secondary input buttons, not the PWR
button handled by the power HAL. A product with three physical buttons
can therefore report `button_count = 2`.
