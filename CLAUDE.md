# CLAUDE.md

Guidance for future Claude Code sessions opening this repo. Read this first.

## Fork status

This is **alecfeeman/Clawdmeter** — a macOS + Waveshare ESP32-S3-Touch-AMOLED-**1.8"** port of the upstream `HermannBjorgvin/Clawdmeter` (Linux + 2.16" board). Hardware drivers, screen layout, and the daemon are all different. Don't follow upstream's README/CLAUDE.md when in doubt — they describe the 2.16" + Linux + BlueZ path.

## Hardware (Waveshare ESP32-S3-Touch-AMOLED-1.8)

- Display: **SH8601** AMOLED via QSPI, **368×448**. Pins: CS=12, SCLK=11, SDIO0..3=4..7. Reset routed through the XCA9554 I/O expander (no direct GPIO).
- Touch: **FT3168** via I2C (SDA=15, SCL=14, INT=21).
- PMU: **AXP2101** on same I2C bus (addr=0x34) — battery, VBUS, PKEY IRQ (short + long).
- IMU: **QMI8658** on same I2C bus (rotation locked to 0 because the panel isn't square).
- Audio: **ES8311** codec via I2S (MCLK=16, BCK=9, WS=45, DO=8, DI=10), amp enable on GPIO 46.
- I/O expander: **XCA9554** at I2C 0x20 — pulses LCD/touch resets at boot.
- Buttons:
  - BOOT (GPIO 0) → toggle display sleep (brightness=0; AMOLED dark = ~no draw).
  - PWR (AXP2101 PKEY) → short: cycle screens / wake display. Long press: `pmu.shutdown()`.
  - GPIO 18 reserved for HID but not exposed on this board variant.

## Architecture

```
main.cpp        — setup(), loop(), button polling, display sleep, attn rising-edge beep
display_cfg.h   — pins, externs for global hardware objects
ui.{h,cpp}      — 4 screens (splash, usage, sessions, settings) + attention overlay
splash.{h,cpp}  — 20×20 pixel-art animation engine, 18× upscale → 360×360 canvas
imu.{h,cpp}     — accelerometer init (rotation tracking disabled for non-square panel)
power.{h,cpp}   — AXP2101 wrapper (battery, charging, PWR short/long press, shutdown)
audio.{h,cpp}   — ES8311 + I2S init, sine-wave beep generator, volume/mute
ble.{h,cpp}     — NimBLE peripheral (no bonding); data + HID keyboard services
data.h          — UsageData struct + SessionInfo for the sessions screen
es8311.{c,h}    — vendored codec driver from Waveshare
icons.h, logo.h, font_*.c — assets
```

Vendored libraries in `firmware/lib/`:
- `Arduino_DriveBus` — Waveshare's FT3168 + IIC chip driver framework.
- `Adafruit_XCA9554` — I/O expander for the reset pulse sequence at boot.

## Build / flash (macOS)

```bash
brew install platformio
pio run -d firmware                                         # build
pio run -d firmware -t upload --upload-port /dev/cu.usbmodemXXXX
```

Find the port with `ioreg -r -c IOSerialBSDClient -l -w 0 | grep IOCalloutDevice`. Note that `ls /dev/cu.usbmodem*` may silently miss it (zsh glob behavior).

## Daemon (host side, macOS)

```
daemon/claude-usage-daemon-macos.py    — bleak-based BLE client, polls Anthropic API
daemon/clawdmeter-hook.py              — Claude Code hook helper (per-session state)
daemon/install-macos.sh                — one-shot installer
```

Flow:
1. CC fires a hook → `clawdmeter-hook.py` writes/removes a file in `/tmp/clawdmeter-sessions/`.
2. Daemon polls `/tmp/clawdmeter-sessions/` every 1s; on any change, sends an updated payload to the board over BLE GATT.
3. Daemon also hits `api.anthropic.com/v1/messages` with a 1-token request every 60s and parses the rate-limit response headers (no Anthropic Code-specific API needed).
4. OAuth token is pulled at runtime from the macOS Keychain (service `Claude Code-credentials`) — never written to disk or logs.

## Critical gotchas

1. **No BLE bonding.** `setSecurityAuth(false, false, false)` in `ble.cpp` — reflashes used to cause "Peer removed pairing information" mismatches because ESP NVS got wiped while macOS retained its half. Trade-off: any nearby device could write fake usage data. HID keyboard service likely won't work without bonding either; revisit if HID is ever needed.
2. **MTU is 517.** `NimBLEDevice::setMTU(517)` — the sessions payload can exceed default 23-byte MTU. macOS CoreBluetooth negotiates an effective ~185–500 in practice.
3. **Rotation is locked to 0.** `imu_get_rotation()` always returns 0 (`imu.cpp`). The 368×448 panel isn't square, so 90/270° rotation crops; the strip-rotation logic in `main.cpp` only runs the `r==0` fast path.
4. **OPI PSRAM** required: `board_build.arduino.memory_type = qio_opi` in `platformio.ini`. Without it, `MALLOC_CAP_SPIRAM` returns NULL and the screen stays black.
5. **pioarduino platform required.** GFX Library for Arduino needs Arduino Core 3.x.
6. **Touch reads are centralized** in `main.cpp:touch_read()`. Both the LVGL pointer indev and any future tap detector must share the `touch_pressed/touch_x/touch_y` globals, not call the driver directly.
7. **Attention overlay is full-screen takeover.** `ui_set_attn()` hides logo + battery + active container, restores them on clear. Triggered by non-empty `at` field in the BLE payload.
8. **Audio init order:** I2S `setPins` + `begin` must happen BEFORE `es8311_create`; codec driver issues I2C reads during init that the I2S clock has to be feeding.
9. **Volume/mute don't persist across reboots** (defaults to 85% / unmuted). Add 4 lines via the `Preferences` library if you want them sticky.

## UI conventions

- Title font: Tiempos 34, vertically centered with the 80×80 logo (which sits top-left).
- Logo (top-left x=12 y=20) and battery icon (top-right x=308 y=30) reserve the corners; titles must avoid x<92 and x>308.
- Bottom of Usage page reserves ~32px for the rotating verb label (`font_mono_18`); panels must end by y≈416.
- Navigation: **swipe left/right** cycles Usage ↔ Sessions ↔ Settings; **swipe up** from any screen shows the splash; **swipe any direction** on splash dismisses. Tap does nothing on main screens (was conflicting with swipes).

## Attribution

Upstream: [HermannBjorgvin/Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter). Clawd pixel-art animations from [claudepix](https://claudepix.vercel.app) by @amaanbuilds.
