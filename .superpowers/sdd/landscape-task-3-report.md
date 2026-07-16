# Landscape Task 3 report

## Status

Complete against base `4aa49ff`.

## Implementation

- Kept the physical panel dimensions fixed at 240x320 and selected the logical
  canvas as 240x320 portrait or 320x240 landscape with `BOARD_LANDSCAPE`.
- Initialized the ST7789 with native dimensions, retained Adafruit rotation
  bookkeeping, and applied board-verified MADCTL values `0x88` for portrait and
  `0xA8` for USB-left landscape.
- Added the USB-left landscape touch transform
  `x = 319 - clamped_raw_y`, `y = clamped_raw_x`.
- Added `esp32_2432s024c_landscape` as an inherited PlatformIO environment.

## TDD evidence

RED:

- `python -m pytest tests/test_esp32_2432s024c_contract.py -q` failed with the
  missing landscape environment and missing `LCD_NATIVE_*` constants.
- Unity compile/link failed with missing `map_touch_to_landscape` and
  `st7789_bgr_madctl` symbols.

GREEN:

- Contract tests: 6 passed.
- Unity `test_port_helpers` compile/link: passed without upload or hardware test.
- Portrait build (`esp32_2432s024c`, `-j 1`): success; RAM 32.3%, flash 41.8%.
- Landscape build (`esp32_2432s024c_landscape`, `-j 1`): success; RAM 32.3%,
  flash 41.8%.

## Self-review

- `git diff --check` passed.
- Changes are limited to the eight Task 3 files plus this report.
- No UI files were changed.
- No serial port was accessed and no process was stopped.

## Concerns

- Hardware orientation and touch behavior were not exercised because this task
  explicitly required compile/link validation without hardware.
- Builds retain pre-existing deprecation warnings from NimBLE-Arduino and
  ArduinoJson, plus the toolchain GNU-stack linker warning; none caused a build
  failure.
