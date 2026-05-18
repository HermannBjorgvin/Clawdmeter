# Project context

ESP32-S3 firmware for a desk-side Claude Code usage monitor. Supports two boards:
- **Waveshare ESP32-S3-Touch-AMOLED-2.16** (480×480 AMOLED) — env `waveshare_amoled_216`
- **Waveshare ESP32-S3-Touch-LCD-4** (480×480 RGB LCD) — env `waveshare_lcd4`

Connects to a host daemon over BLE; daemon polls Anthropic API for usage data.

This file is for future Claude Code sessions to bootstrap quickly. Read this first.

## Hardware (critical pins)

### AMOLED-216
- Display: **CO5300** AMOLED via QSPI (CS=12, SCLK=38, SDIO0..3=4..7, RST=2)
- Touch: **CST9220** via I2C (SDA=15, SCL=14, INT=11, addr=0x5A)
- PMU: **AXP2101** on same I2C bus (addr=0x34) — battery, USB VBUS, PWR button IRQ
- IMU: **QMI8658** on same I2C bus (addr=0x6B) — accelerometer for auto-rotation
- Buttons: GPIO 0 (Space/PTT), GPIO 18 (Shift+Tab), AXP PKEY (cycle screens/animations)

### LCD-4
- Display: **ST7701** RGB parallel (DE=40, VSYNC=39, HSYNC=38, PCLK=41, R0-4=46/3/8/18/17, G0-5=14/13/12/11/10/9, B0-4=5/45/48/47/21); ST7701 init via SPI (CS=42, SCK=2, MOSI=1)
- Touch: **GT911** via I2C (SDA=15, SCL=7), polled (no IRQ pin)
- IO expander: **addr 0x24** on I2C — must init before `gfx->begin()` (regs 0x02=0xFF, 0x03=0x3A)
- Buttons: GPIO 0 only — short press = Space, long press ≥700ms = cycle screens/animations
  (RST button is hardware-only; no PMU/IMU on this board)

## Architecture

```text
main.cpp        — setup(), loop(), button polling (left→Space, right→Shift+Tab, mid→cycle), rotation flash
display_cfg.h   — pin defines, extern object decls
ui.{h,cpp}      — 3-screen UI (splash, usage, bluetooth); splash is touch-toggled, usage↔bluetooth via mid button
splash.{h,cpp}  — 20×20 pixel-art animation engine, 24× upscale to 480×480
imu.{h,cpp}     — accelerometer-driven rotation tracker (returns 0..3)
power.{h,cpp}   — AXP2101 wrapper (battery %, charging, VBUS, PWR button)
touch.{h,cpp}   — minimal tap detector → ui_toggle_splash() (Usage/Splash) or ble_clear_bonds() (BT reset zone)
ble.{h,cpp}     — NimBLE peripheral: custom data service + HID keyboard
data.h          — UsageData struct
icons.h         — icon arrays. Battery (5×) are RGB565A8 with alpha; rest are raw RGB565.
logo.h          — 80×80 RGB565 logo
font_*.c        — pre-compiled LVGL 9 bitmap fonts (Tiempos 56, Styrene 48/28/24/20, Mono 32)
splash_animations.h — generated, do not hand-edit
```

## Build / flash

```bash
pio run -d firmware -e waveshare_amoled_216                                              # build AMOLED
pio run -d firmware -e waveshare_lcd4                                                    # build LCD-4
pio run -d firmware -e <env> -t upload --upload-port /dev/ttyACM0                       # flash Linux
./flash-mac.sh <env>                                                                     # flash macOS
./flash-mac.sh <env> /dev/cu.usbmodem1101                                               # flash macOS explicit port
```

Available envs: `waveshare_amoled_216`, `waveshare_lcd4`. The `-e` flag is required — omitting it builds/flashes the wrong env.

Device shows up as `/dev/ttyACM0` on Linux (Espressif USB JTAG/serial debug unit). No boot-mode gymnastics needed — direct flash works.

## QA your own UI changes — don't ask the user

The firmware ships a `screenshot` serial command that dumps the LVGL framebuffer over `/dev/ttyACM0`. `./screenshot.sh out.png /dev/ttyACM0` captures a 480×480 PNG. **Use this on every UI iteration** — Read the PNG with the Read tool, verify the change visually, iterate.

The boot screen is `SCREEN_SPLASH` and only advances on a physical button press, so a fresh flash will sit on the splash. To screenshot the screen you're actually editing without asking the user to press a button, **temporarily change the default boot screen** in `main.cpp` (search for `ui_show_screen(SCREEN_SPLASH);`) to `SCREEN_USAGE` / `SCREEN_CONTROLLER` / `SCREEN_BLUETOOTH`, do your iteration, then revert before committing.

## Critical gotchas

1. **CO5300 cannot rotate.** Its MADCTL only supports axis flips, not column/row exchange. Rotation is done by **CPU pixel remapping in `my_flush_cb`** in main.cpp. We use **PARTIAL render mode with strip rotation** (small 480×40 strips, fast). On rotation change → AMOLED brightness flash → force redraw.
2. **OPI PSRAM required on both boards.** `board_build.arduino.memory_type = qio_opi` in platformio.ini. Both the AMOLED-216 and LCD-4 use ESP32-S3R8 with OPI PSRAM. Using `qio_qspi` causes `MALLOC_CAP_SPIRAM` to return NULL and the screen is black.
3. **pioarduino platform required.** GFX Library for Arduino needs Arduino Core 3.x (`esp32-hal-periman.h`), not the 2.x that standard `espressif32` ships. We pin `pioarduino/platform-espressif32` 55.03.38-1.
4. **LVGL 9 font patching.** `lv_font_conv` outputs LVGL 8 format. Must remove `#if LVGL_VERSION_MAJOR >= 8` guards, drop `.cache` field, add `.release_glyph`, `.kerning`, `.static_bitmap`, `.fallback`, `.user_data`. Without patching, fonts render invisible.
5. **Touch reading must be centralized.** CST9220's `getPoint()` does a full I2C transaction. Calling it from multiple places consumed each other's data and broke input. `touch_read()` is called once per loop in main.cpp; both LVGL `my_touch_cb` and `touch.cpp` read from shared `touch_pressed/touch_x/touch_y` state.
6. **CO5300 needs even-aligned flush regions.** `rounder_cb` enforces this (AMOLED only; not needed for ST7701).
7. **Touch `setSwapXY(true)` and `setMirrorXY(true, false)`** are the empirically-correct values for the AMOLED's CST9220 at default rotation 0. IMU rotation logic doesn't change touch mapping (it does CPU-side rotation of the rendered pixels, so LVGL still thinks the display is portrait at 0°).
8. **LVGL RGB565A8 is planar.** `w*h` RGB565 pixels followed by `w*h` alpha bytes; `data_size = w*h*3`, `stride = w*2`. Use `init_icon_dsc_rgb565a8()` for icons that overlap non-uniform backgrounds (e.g. battery over splash). Lucide source PNGs are black-on-transparent — converter must tint to white or icons render invisible. See `tools/png_to_lvgl.js`.
9. **LCD-4 IO expander must init before `gfx->begin()`.** The TCA9554-style expander at I2C 0x24 controls display power rails. Write reg 0x02=0xFF (outputs high) then reg 0x03=0x3A (direction) before calling `gfx->begin()`, or the display stays dark.
10. **LCD-4 RGB panel tearing fix: bounce buffers.** `Arduino_RGB_Display` DMA-scans directly from PSRAM, causing write races. We pass `bounce_buffer_size_px = LCD_WIDTH * 10` to `Arduino_ESP32RGBPanel` so ESP-IDF allocates two ~9.4 KB SRAM bounce buffers; DMA reads from SRAM, CPU writes to PSRAM freely. Do not call `rgbpanel->getFrameBuffer()` after `gfx->begin()` — it calls `esp_lcd_new_rgb_panel()` a second time and crashes (no free slot).
11. **LCD-4 has only one user button (GPIO 0 / BOOT).** GPIO 18 is display R3. The KEY/PWR button is wired to EN/RST (hardware reset), not a GPIO. `BTN_FWD` is not defined for the LCD-4 build.

## Icons

`tools/png_to_lvgl.js <input.png> <symbol> [W_MACRO] [H_MACRO] [--tint=RRGGBB | --no-tint]` converts an alpha PNG to RGB565A8. Default tint is white (`0xFFFFFF`) — necessary for Lucide PNGs. Splice output into `firmware/src/icons.h` and use `init_icon_dsc_rgb565a8()` in ui.cpp. Currently only the 5 battery icons use this format; the rest are still raw RGB565 baked over the panel background, fine because they live inside opaque zones.

## Splash animations

13 × 20×20 pixel-art creature animations sourced from
[claudepix.vercel.app](https://claudepix.vercel.app). Pipeline:

```bash
node tools/scrape_claudepix.js  # → tools/claudepix_data/*.json
node tools/convert_to_c.js      # → firmware/src/splash_animations.h
```

Each animation has a per-animation 10-color RGB565 palette. Cell values 0..9 index it. Default boot screen.

## User profile / preferences

See `~/.claude/projects/.../memory/` files for persistent context (user is an embedded-beginner senior dev, brand-conscious, prefers iterative UI refinement, dislikes me authoring my own art when third-party assets are intended). Always read those memory files at session start.

## Recent session highlights

- Migrated from Panlee SC01 Plus (480×320 IPS) to Waveshare 2.16" AMOLED (480×480 square). Full hardware/library swap.
- Added IMU auto-rotation, battery indicator, USB-state-aware screen switching.
- Added splash screen with scraped pixel-art animations and 3-button physical input layout.
- Fonts and icons re-scaled ~1.9× for the higher-DPI panel.
- All UI margins widened to 20px to clear the rounded display corners.
- Battery icons converted to RGB565A8 alpha so they blend cleanly over the splash animations.
- Added Waveshare ESP32-S3-Touch-LCD-4 support (`waveshare_lcd4` env). ST7701 RGB parallel panel with GT911 touch; AXP2101/QMI8658 absent (stubs in power.cpp/imu.cpp). Bounce buffers eliminate DMA tearing. Single-button UX: short press = Space, long press = cycle screens.

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
