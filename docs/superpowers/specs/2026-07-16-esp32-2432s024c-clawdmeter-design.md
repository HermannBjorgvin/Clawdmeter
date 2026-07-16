# Clawdmeter on ESP32-2432S024C Design

## Goal

Run the current Clawdmeter firmware and Windows daemon on Gustavo's verified
ESP32-2432S024C so the device can remain USB-powered as a desk display for
Claude Code session and weekly usage.

## Verified target hardware

- MCU: ESP32-D0WD-V3 revision 3.1, dual core, 240 MHz.
- Flash: 4 MB.
- PSRAM: none.
- Upload interface: CH340 on COM3.
- Display: ST7789, 240 x 320, SPI.
- Display pins: SCLK 14, MOSI 13, MISO 12, CS 15, DC 2, no reset pin.
- Backlight: GPIO 27, active high and PWM-capable.
- Touch: CST816/CST820-compatible controller at I2C address 0x15.
- Touch pins: SDA 33, SCL 32, reset 25. The validated reader polls six bytes
  beginning at register 0x01 and does not require the interrupt pin.
- BOOT button: GPIO 0.
- Onboard RGB LED: GPIO 4, 16, and 17, active low; not required by Clawdmeter.
- No PMU, battery gauge, IMU, automatic rotation, or secondary user button.

The previous Hall-probe firmware visually validated the ST7789 path and read
touch events from this exact device. Public ILI9341 definitions for similarly
named boards must not override this device-specific evidence.

## Architecture

The port will use Clawdmeter's current board HAL instead of copying the closed
legacy CYD pull request. Shared application code remains shared. A new
`firmware/src/boards/esp32_2432s024c/` directory will implement display, touch,
input, power, sound, IMU, board initialization, and capability reporting.

Only two shared-code changes are allowed:

1. Add a 240 x 320 responsive layout breakpoint and make currently hard-coded
   usage fonts and status sizes layout-driven.
2. Add a compile-time option to omit BLE HID only if hardware validation shows
   Windows cannot maintain the standard bonded connection without usable HID
   buttons. The initial implementation keeps the upstream bonded BLE behavior.

## PlatformIO environment

Add `[env:esp32_2432s024c]` to `firmware/platformio.ini` with:

- the same pinned pioarduino ESP32 platform used by upstream;
- `board = esp32dev`;
- Arduino framework;
- 4 MB flash and `huge_app.csv` partition layout;
- upload speed 460800 and monitor speed 115200;
- no USB CDC flags and no `BOARD_HAS_PSRAM` flag;
- a board-specific `build_src_filter`;
- existing Arduino GFX, LVGL, ArduinoJson, and NimBLE dependencies;
- no additional touch dependency: use the validated minimal I2C reader.

The large application partition intentionally gives up OTA partitions. Firmware
updates for this desk device will use the CH340 USB connection.

## Board HAL design

### Display

`display.cpp` will construct an Arduino GFX ST7789 display on HSPI with the
verified pins. Clawdmeter will use portrait geometry, 240 x 320. The first smoke
test will confirm rotation, color order, and mirroring because the earlier Hall
UI was validated in 320 x 240 landscape mode.

The LVGL flush path will send RGB565 strips. Backlight brightness will use PWM
on GPIO 27 so the existing four saved brightness levels continue to work.

### Touch

`touch.cpp` will reset the controller on GPIO 25, start I2C at 400 kHz on
GPIO 33/32, read the validated register layout, and map raw coordinates into the
portrait display orientation. It must return promptly without blocking LVGL.
Coordinate mapping will be isolated in a pure helper so host tests can verify
corners, clamping, axis swaps, and mirroring before hardware flashing.

### Power and input

There is no battery or PMU. Battery percentage is `-1`, charging is false, and
VBUS is reported as present because this target is USB-powered. This works with
the upstream `IDLE_SLEEP_WHEN_CHARGING=false` rule to keep the display awake.

GPIO 0 will provide the PWR-style control contract rather than HID typing:

- short press on splash: next animation;
- short press on usage: cycle brightness;
- hold about three seconds and release: clear BLE bonds and advertise again.

`input_hal` reports no primary or secondary HID key presses. This avoids sending
Space when the same BOOT button is used for device control. The BLE HID service
remains initially enabled for compatibility with the current Windows pairing
flow, even though the board exposes no typing buttons.

### Unsupported capabilities

IMU, sound, battery, secondary button, and rotation implementations are no-ops.
Their capability flags are false so unused assets and behavior can be removed by
the linker where possible.

## 240 x 320 UI

Add a third layout profile below the existing 368 x 448 compact breakpoint.
The profile will keep both usage panels visible in portrait mode with:

- margins around 10 pixels;
- a smaller title/header region;
- two vertically stacked panels sized to leave room for the bottom status line;
- smaller percentage, reset, pill, and status fonts already present in the
  repository (`font_tiempos_34`, Styrene 12/14/16/20, and `font_mono_18`);
- a smaller idle creature.

The splash renderer already has a no-PSRAM path capped at a 200 x 200 RGB565
canvas. Keep that cap to preserve heap for LVGL and NimBLE. The splash remains
centered with black borders.

The device boots into the upstream splash screen. Tapping anywhere toggles
between splash and usage. The usage screen continues to show the upstream idle
state after 90 seconds without fresh daemon data; retaining stale numeric values
is outside this port's scope.

## Windows daemon

Use the upstream native Windows tray daemon without functional changes. The
installer creates `.venv`, installs `bleak`, `httpx`, `pystray`, and `Pillow`,
registers per-user login startup, and launches the tray application.

The device must be paired once as `Clawdmeter`. The daemon reads the existing
Claude Code credential file, polls usage every 60 seconds, and writes the JSON
payload over BLE GATT. The computer must be logged in, awake, and within BLE
range for live values. The display may be powered from a wall USB supply after
flashing.

## Testing and verification

Development follows red-green TDD where host-side behavior is testable:

1. Add a failing source-contract test for the new PlatformIO environment and
   board capability/pin definitions.
2. Add failing pure tests for portrait touch coordinate mapping.
3. Add a failing source/layout test proving two 240 x 320 panels fit vertically
   and small fonts are selected.
4. Implement only enough code to make each test pass.
5. Run the upstream daemon tests.
6. Build `pio run -d firmware -e esp32_2432s024c` from a clean state.
7. Upload to COM3 and capture serial initialization evidence.
8. Confirm visually that the splash and usage screens render with correct
   orientation and colors; confirm multiple touch locations map correctly.
9. Pair via Windows Bluetooth, install the tray daemon, and verify a real GATT
   payload updates session and weekly values on the physical display.
10. Leave the device powered long enough to verify that it does not sleep while
    on USB.

Hardware verification is required before claiming the port works. A successful
compile alone is not sufficient.

## Error handling

- Display, touch, and BLE initialization print concise serial diagnostics.
- Touch read failures report no touch for that frame and do not block rendering.
- Allocation failure leaves a serial error and avoids dereferencing null buffers.
- Daemon token, HTTP, and BLE failures use the upstream tray status and log.
- If automatic upload cannot enter the bootloader, the fallback is holding BOOT
  while resetting or reconnecting USB; no flash erase occurs unless explicitly
  needed for BLE bond recovery.

## Acceptance criteria

- PlatformIO Core is installed and callable as `pio`.
- The new environment builds successfully from the repository root.
- Firmware uploads through COM3 without changing board flash size assumptions.
- Splash and usage screens are fully visible on 240 x 320.
- Touch toggles screens with correct portrait coordinates.
- The display remains awake while USB-powered.
- Windows pairs with `Clawdmeter` and the tray daemon starts at login.
- A real usage payload updates both session and weekly values on the device.
- Upstream daemon tests and new port tests pass.

## Out of scope

- Codex/OpenAI usage data from the closed CYD pull request.
- Wi-Fi transport, OTA updates, SD card, RGB LED effects, speaker support, and
  battery operation.
- Publishing or redistributing proprietary Anthropic fonts and mascot assets
  beyond the existing upstream repository.
