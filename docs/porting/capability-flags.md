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

## Runtime-only BoardCaps fields

A few `BoardCaps` fields don't have a matching `BOARD_HAS_*` macro
because no per-board source needs to dead-strip code on them — they're
runtime hints to shared code. Set them directly in `caps.cpp`:

| Field             | Default | What it gates |
|-------------------|---------|---------------|
| `has_touch`       | `false` | Whether the board has a capacitive touchscreen. When `false`, shared code routes a PWR-button press on the splash screen to `ui_toggle_splash()` instead of `splash_next()`, so users without a touchscreen can still leave the splash. The AMOLED ports set `true`; the Xingzhi sets `false`. |
| `auto_cycle_ms`   | `0`     | Optional global "rotate screens" timer in milliseconds. When `> 0`, `main.cpp` calls `ui_cycle_screen()` at that cadence (skipping splash + pausing on idle). Touchless boards typically set 5000 ms so the user doesn't have to mash buttons to see every screen; touch-capable boards leave it at 0. |
| `cycle_skip_bluetooth` | `false` | When `true`, the Bluetooth screen is omitted from `ui_cycle_screen()`'s rotation — useful once first-time pairing is done and the screen has no day-to-day value. The Bluetooth screen is still reachable via a direct `ui_show_screen(SCREEN_BLUETOOTH)` call, so a long-press / dedicated button can be wired up later if needed. |

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
