# Add a board

A port consists of `firmware/src/boards/<name>/` and a PlatformIO environment.
Board-specific drivers implement the [HAL contract](hal-contract.md).
Shared code uses capabilities, not board-name conditionals. If the HAL
cannot express a required behavior, propose an interface change before
adding special cases to `main.cpp` or `ui.cpp`.

Before starting, check for an existing pull request for the board. Testing
the same hardware helps more than a duplicate port.

## Choose a reference

The supported ports include ESP32-S3 boards with PSRAM and ESP32-C6 boards
without it. Start from a board with the same SoC and display bus. Do not
copy S3 memory settings into a C6 environment.

| Hardware | Reference |
| --- | --- |
| QSPI AMOLED with software rotation | `waveshare_amoled_216` |
| Two panel revisions and an IO expander | `waveshare_amoled_18` |
| C6 without PSRAM | `waveshare_amoled_216_c6`, `waveshare_amoled_18_c6` |
| Watch-size panel and direct reset GPIOs | `waveshare_amoled_206` |
| SPI TFT, PWM backlight, ADC battery | `waveshare_lcd_154` |
| RGB parallel panel and expander backlight | `waveshare_lcd_4` |

The board needs a supported display driver, touch input, and a primary
button. Battery monitoring, a secondary button, rotation, and sound are
optional. Implement no-op HAL functions for absent features and declare
the corresponding capabilities accurately.

## Implement the port

1. Copy `firmware/src/boards/template` to a new board folder.
2. Fill in `board.h`: pins, I2C addresses, display dimensions, and flags.
   Keep these local to the board.
3. Implement `board_init.cpp` and the display, touch, input, power, IMU,
   sound, and capability sources. Release reset lines and enable power
   rails before initializing the display or touch controller.
4. Copy a compatible PlatformIO environment. Keep its shared build settings,
   exclude `boards/`, then include only your board folder. Set the identity
   and `CLAWD_ENV` definitions using the existing environments as examples.
   Choose flash size, partitions, and PSRAM settings from the actual hardware.
5. Build with `pio run -d firmware -e my_board`. Resolve missing or duplicate
   HAL symbols before flashing.
6. Flash the target and inspect startup diagnostics. The device should boot
   to the meters. Check touch coordinates, buttons, battery state, Bluetooth,
   brightness, and any supported audio or rotation.
7. Review all device states at the panel's native resolution. On PSRAM boards,
   `./screenshot.sh out.png [port]` captures the framebuffer. C6 builds disable
   snapshots; inspect the physical display. Simulator captures supplement
   hardware checks but do not replace them.

## Include the board in the app and docs

A new port must include:

- An original SVG at
  `desktop/crates/clawdmeter-app/assets/boards/<env>.svg`. Draw the real
  product in a 3/4 view, matching the existing illustrations. Use product
  photos as reference, not as copied or embedded artwork.
- An entry with the display name and buy link in
  `desktop/crates/clawdmeter-app/src/boards.rs`, plus the SVG registration
  in `desktop/crates/clawdmeter-app/src/ui/mod.rs`.
- An entry with the display name and buy link in the README's
  **Supported boards** table. Waveshare product links must include
  `?aff_id=149786` (or `&aff_id=149786` when a query already exists).
  Other vendors use a plain product link. Apply this to the app entry too.
- A display-name entry in `tools/ci/make_manifest.py`'s `DISPLAY_NAMES`.
  Confirm that the environment, release manifest, and app catalog agree.

In the PR, include the buy link and preferably a photo of the actual board
running Clawdmeter. If no photo is available, say so. State the exact board
and revision tested on hardware and what you verified; simulator captures
are not hardware testing. Use the PR template for the hardware-testing and
human-involvement notes.

## Common failures

| Symptom | Check |
| --- | --- |
| Black screen | Required PSRAM settings on S3, enabled power rails, expander state before `gfx->begin()`, reset sequence, and driver support. C6 must use internal-memory paths. |
| Vertical strip at a CO5300 panel edge | Verify the controller's column offset. Current references use 0 on S3 2.16, 16 on the CO5300 S3 1.8 revision, and 23 on S3 2.06. Do not assume one value fits every panel. |
| Incorrect touch position | Apply coordinate swap and mirroring in `touch.cpp`. Read the controller in one place so competing I2C reads cannot consume the same event. |
| RGB tearing or crash | Follow the LCD-4 bounce-buffer setup. Do not create a second RGB panel by calling `getFrameBuffer()` after initialization. |
| Battery brownout on LCD-1.54 | Drive its power-hold line HIGH early in board initialization. |
| Wrong implementation linked | Check `build_src_filter` order: exclude all board folders, then include exactly one. |
| Animation clips | Inspect `compute_layout()` at the actual dimensions. Keep display-size decisions there; use capabilities for hardware differences. |

Choose drivers compatible with the project's dependency policy. A small
independently implemented I2C reader may be appropriate when no suitable
library is available; copying a driver's code does not remove its license.
