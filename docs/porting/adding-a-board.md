# Porting Clawdmeter to a new board

A board port is a folder under `firmware/src/boards/` plus a new
`[env:...]` block in `firmware/platformio.ini`. You should never need
to edit `firmware/src/main.cpp`, `firmware/src/ui.cpp`, or anything
under `firmware/src/hal/`. If you find yourself wanting to, that's a
gap in the HAL — open an issue.

## Hardware you need

At minimum:

- An **ESP32**. The upstream firmware runs on the S3 (most ports), the
  C6 (RISC-V, no PSRAM), and the original ESP32 classic (M5Stack FIRE) —
  so any family member the pioarduino Arduino 3.x core supports is fair
  game. **PSRAM strongly recommended**: partial flush buffers and the
  splash canvas allocate from it by default. Boards without PSRAM must
  build without `-DBOARD_HAS_PSRAM` (shared code then shrinks the buffers
  into internal SRAM and disables the `screenshot` command — see the C6
  envs).
- A **panel** with a driver in
  [GFX Library for Arduino](https://github.com/moononournation/Arduino_GFX).
  QSPI AMOLEDs (CO5300, SH8601, NV3041A) and plain 4-wire SPI TFTs
  (ST7789 on the LCD-1.54, ILI9342C on the M5Stack FIRE) both work — the
  bus is abstracted behind the driver, so the display HAL surface is the
  same either way.
- **Either** a touch controller over I2C **or** enough physical buttons
  to navigate. Touch is optional: the M5Stack FIRE has none, and its
  `touch.cpp` is a permanent no-op (`read` always reports "not pressed")
  while the three front buttons carry all input. A touch driver, if you
  have one, just needs init + read.
- A **primary button** (typically the BOOT/GPIO 0 push button).

Optional:

- A second physical button (e.g. for HID Shift+Tab mode toggle).
- An AXP2101 PMU for battery monitoring + a power button.
- A QMI8658 (or compatible) IMU for automatic rotation.
- An XCA9554 / PCA9554 IO expander if reset / enable lines are routed
  through one (the AMOLED-1.8 board does this).

## Step-by-step

1. **Copy the template folder.**

   ```bash
   cp -r firmware/src/boards/template firmware/src/boards/my_board
   ```

2. **Fill in `boards/my_board/board.h`.** Replace every `TODO` with your
   board's pins, I2C addresses, dimensions, and capability flags. The
   capability flags drive both compile-time dead-stripping in the HAL
   implementations and runtime UI decisions via `BoardCaps`.

3. **Implement the per-board sources.** Each one corresponds to a HAL
   header in `firmware/src/hal/`. Look at one of the reference ports for
   a worked example:

   | File              | Reference port (start here)                                    |
   |-------------------|----------------------------------------------------------------|
   | `display.cpp`     | `boards/waveshare_amoled_216/display.cpp` (with CPU rotation) or `_18/display.cpp` (no rotation) |
   | `touch.cpp`       | `_216/touch.cpp` (library-based) or `_18/touch.cpp` (vendored I2C reader) |
   | `input.cpp`       | `_216/input.cpp` (two buttons) or `_18/input.cpp` (one button) |
   | `power.cpp`       | `_216/power.cpp` (PMU IRQ) or `_18/power.cpp` (PMU + IO expander button) |
   | `imu.cpp`         | `_216/imu.cpp` (full rotation) or `_18/imu.cpp` (init-only stub) |
   | `caps.cpp`        | either reference — just edit the struct literal               |
   | `board_init.cpp`  | `_216/board_init.cpp` (no expander) or `_18/board_init.cpp` (with expander) |

4. **Add a PlatformIO env.** In `firmware/platformio.ini`, copy one of
   the existing `[env:waveshare_amoled_*]` blocks and adjust:

   ```ini
   [env:my_board]
   ; ... platform / board / framework as before ...

   build_src_filter =
       +<*>
       -<boards/>
       +<boards/my_board/>          ; the only line you change here

   build_flags =
       -DBOARD_MY_BOARD             ; identity-only — the shared code never
                                     ; branches on this; per-board code may
   ```

   If your panel needs flash > 4 MB (extra animations, larger fonts),
   copy the `board_upload.*` block from the AMOLED-1.8 env.

5. **Build.** `pio run -d firmware -e my_board`. The link step is the
   real verification — any missing HAL symbol or duplicated definition
   shows up here.

6. **Flash + smoke test.** The first boot should land on the splash
   screen. If it doesn't, check `pio device monitor` for HAL init
   messages — every reference port logs OK / failure for display, touch,
   PMU, IMU during `setup()`.

7. **Visual QA.** `./screenshot.sh out.png` over USB serial captures
   the live framebuffer at the active resolution. The UI is responsive
   (see [hal-contract.md](hal-contract.md) for breakpoint details);
   most ports will look acceptable out of the box. If your screen size
   doesn't match an existing breakpoint, you may want to add one to
   `compute_layout()` in `firmware/src/ui.cpp`.

## Common pitfalls

- **Display stays black, no panic.** Usually one of: OPI PSRAM not enabled
  in platformio.ini (check `board_build.arduino.memory_type = qio_opi`);
  IO expander not released before `gfx->begin()` (run `io_expander_init()`
  from `board_init()`); GFX library version too old to know about your
  panel chip; reset line not pulsed before `gfx->begin()` (do it in
  `board_init()` for direct-GPIO resets, or via the IO expander otherwise).
- **Display works but a vertical strip of garbage/stale content shows on
  one edge.** CO5300-based panels expose their visible viewport at a
  horizontal offset inside the controller's internal RAM, and the offset
  varies per physical panel size. The 2.16" port uses `col_offset1 = 0`,
  the 2.06" uses `col_offset1 = 23`. When adding a new CO5300 board,
  grab Waveshare's reference value from their `Mylibrary/pin_config.h`
  (or equivalent) and fine-tune ±1 if centering looks off. SH8601 panels
  don't have this issue.
- **Touch reads zeros / wrong coordinates.** The HAL hands LVGL whatever
  the controller reports — apply any axis swap / mirror inside your
  `touch.cpp`. CST9220 needs `setSwapXY(true)` + `setMirrorXY(true,
  false)` on the AMOLED-2.16 board; your controller will likely differ.
- **GPL warning when picking a touch driver.** The project intentionally
  avoids copyleft dependencies. If the only available library is GPL,
  vendor a minimal I2C reader instead (see `_18/touch.cpp`).
- **Both boards built fine but one runs and the other doesn't.** The
  build_src_filter is per-env — re-check you copied the existing env
  blocks correctly and the `-<boards/>` then `+<boards/your_one/>`
  ordering is right (filters apply in declaration order).
