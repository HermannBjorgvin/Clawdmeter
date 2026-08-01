# Capability flags

Each board's `board.h` declares these. They're consumed in two places:

1. **`caps.cpp`** — copies them into the `BoardCaps` instance so shared
   code (`ui.cpp`, `main.cpp`) can query them at runtime via
   `board_caps()`.
2. **The per-board source files** — `#if BOARD_HAS_*` lets the linker
   dead-strip entire functions on boards that don't need them.

Keep the two in sync. The pattern in `caps.cpp` does this for you:
```c
.button_count = (uint8_t)(1 + BOARD_HAS_SECONDARY_BUTTON),
.has_rotation = (bool)BOARD_HAS_ROTATION,
```

## The flags

| Macro                          | Default | What it gates |
|--------------------------------|---------|---------------|
| `BOARD_HAS_SECONDARY_BUTTON`   | 0       | A second physical button (HID Shift+Tab on the reference ports). `caps.button_count = 1 + this`. UI uses `caps.button_count >= 2` to decide whether to poll/handle the secondary button — there is no `#ifdef` in shared code. |
| `BOARD_HAS_ROTATION`           | 0       | IMU-driven auto-rotation via CPU strip transformation in `display_hal_draw_bitmap`. When 0, `display_hal_tick` is a no-op and the rotation buffer in `display.cpp` doesn't get allocated. |
| `BOARD_HAS_IMU`                | 0       | Whether the accelerometer is populated and initialized. Distinct from `BOARD_HAS_ROTATION` — the AMOLED-1.8 has the QMI8658 (so `HAS_IMU=1`) but the kit's enclosure mounts the panel at a fixed orientation, so rotation is off. |
| `BOARD_HAS_BATTERY`            | 0       | Whether PMU battery measurement is meaningful on this board. UI hides the battery indicator when false. |
| `BOARD_HAS_IO_EXPANDER`        | 0       | Whether an IO expander gates display / touch reset lines. Doesn't directly gate any code path — but signals to the porter that `board_init()` must release the expander before `display_hal_init()`. |
| `BOARD_HAS_SESSION_VIEWS`      | 0       | The live-session chat views (issue #135): the one-chat card and the sorted chat list inside the usage screen. Needs room for ~3 cards plus a panel/refresh model that tolerates animation. **Consumed by shared code (`ui.cpp`, `main.cpp`), so unlike the other flags it must also be set as a `-DBOARD_HAS_SESSION_VIEWS=1` build flag in the board's `platformio.ini` env** (same mechanism as `BOARD_HAS_PSRAM`); keep the env flag and `board.h` in agreement. When 0, the card pool, animations, and session parsing aren't compiled at all and the view resolver keeps today's three-way pairing / no-data / quota behavior. |

## Build-flag macros

`BOARD_HAS_PSRAM` is set as a `-D` build flag in `platformio.ini` (not in `board.h`) on chips with external PSRAM wired up. Shared code (`main.cpp`, `splash.cpp`) and per-board display drivers use it to choose between `MALLOC_CAP_SPIRAM` (large buffers) and `MALLOC_CAP_INTERNAL` (small buffers, partial-render LVGL, splash canvas capped at ~80 KB, screenshot capture disabled). New ESP32-C6 / ESP32-C3 ports must leave this undefined.

## Future capabilities

Add a new flag when:

- A shared-code decision currently uses `if (caps.has_<thing>)` and
  you want to extend it (e.g. add `BOARD_HAS_HAPTIC` for vibration
  feedback).
- A per-board file conditionally compiles a block of code (e.g.
  audio amp init under `BOARD_HAS_AUDIO`).

Don't add a flag for a one-off detail. If only one board cares about it
and shared code never queries it, leave it as a constant in that board's
`board.h` and use it only in that board's `.cpp` files.
