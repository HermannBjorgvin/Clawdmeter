# Porting Clawdmeter to a new board

A board port is a folder under `firmware/src/boards/` plus a new
`[env:...]` block in `firmware/platformio.ini`. You should never need
to edit `firmware/src/main.cpp`, `firmware/src/ui.cpp`, or anything
under `firmware/src/hal/`. If you find yourself wanting to, that's a
gap in the HAL — open an issue.

## Hardware you need

At minimum:

- An **ESP32-S3** (other ESP32 family members may work; this is what the
  upstream firmware is tested on). OPI PSRAM is **required** — partial
  flush buffers and the splash canvas are allocated from PSRAM.
- A **display panel**. The two easiest paths:
  - A QSPI **AMOLED panel** with a driver supported by
    [GFX Library for Arduino](https://github.com/moononournation/Arduino_GFX)
    (CO5300, SH8601, NV3041A, etc.). The Waveshare ports use this.
  - A **SPI TFT / non-AMOLED panel** where Arduino_GFX doesn't have a
    matching driver — vendor a hand-rolled minimal driver in your
    board's `display.cpp`. The Xingzhi port shows ~190 lines doing
    init-sequence + CASET/RASET + RAMWR on an NV3023 over plain SPI.
- A **primary button** (typically the BOOT/GPIO 0 push button).

Optional:

- A **touch controller** over I2C — when absent, set
  `BoardCaps.has_touch = false` so shared code routes the PWR-button
  press on the splash screen straight to `ui_toggle_splash()`.
- A second physical button (e.g. for HID Shift+Tab mode toggle).
- A third button to act as PWR / cycle-screens (the Xingzhi remaps
  VOL_UP into `power_hal` for this).
- An AXP2101 PMU for battery monitoring + a power button. Bare Li-Po
  + ADC voltage divider also works — see Xingzhi's `power.cpp`.
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
   | `display.cpp`     | `boards/waveshare_amoled_216/display.cpp` (Arduino_GFX + CPU rotation), `_18/display.cpp` (Arduino_GFX, no rotation), or `xingzhi_cube_183/display.cpp` (hand-rolled SPI init for a panel Arduino_GFX doesn't know) |
   | `touch.cpp`       | `_216/touch.cpp` (SensorLib), `_18/touch.cpp` (vendored I2C reader), or `xingzhi_cube_183/touch.cpp` (stub for no-touch boards) |
   | `input.cpp`       | `_216/input.cpp` (two buttons) or `_18/input.cpp` (one button) |
   | `power.cpp`       | `_216/power.cpp` (AXP2101 + PMU IRQ), `_18/power.cpp` (AXP2101 + IO expander button), or `xingzhi_cube_183/power.cpp` (no PMU — bare ADC + CHRG GPIO + GPIO-edge PWR button) |
   | `imu.cpp`         | `_216/imu.cpp` (full rotation), `_18/imu.cpp` (init-only stub), or `xingzhi_cube_183/imu.cpp` (full stub when there's no IMU) |
   | `caps.cpp`        | any reference — just edit the struct literal                  |
   | `board_init.cpp`  | `_216/board_init.cpp` (no expander), `_18/board_init.cpp` (with expander), or `xingzhi_cube_183/board_init.cpp` (no I2C peripherals, just a power-latch GPIO) |

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
  panel chip.
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
