# Xingzhi Cube 1.83 Bring-Up Issues

This file tracks issues found while adding support for the xingzhi-cube 1.83" 2mic ESP32-S3 board.

## Scope
- Repository: Clawdmeter
- Branch: generic-board-support
- Target env: xingzhi_cube_183

## Issue Log

### 1) Display stayed blank (sometimes only a white line)
- Symptom: backlight on, panel mostly blank, occasional white line at top.
- Root cause: panel requires a vendor-specific init command table before standard ST7789 use.
- Evidence: board YAML and ORC board notes.
- Fix: injected vendor init sequence in custom ST7789 subclass.
- Status: fixed.

### 2) Vendor init got wiped after injection
- Symptom: panel still unstable after adding vendor commands.
- Root cause: Arduino_ST7789::tftInit() always sends SWRESET (0x01), which clears vendor register programming.
- Fix: override tftInit() and do full init without calling base tftInit().
- Status: fixed.

### 3) Wrong colors (expected orange, looked white-ish)
- Symptom: accent colors looked wrong/washed.
- Root cause candidates: panel color mode/order and init sequencing.
- Fixes applied:
  - set COLMOD to RGB565 in vendor path.
  - preserve vendor sequence ordering.
  - keep known-good invert setting for this panel.
  - apply xingzhi-only orange tint mapping in splash render path.
- Status: fixed.

### 4) Initial suspicion of offset issue
- Symptom: possible clipping/partial render looked like offset problem.
- Root cause: mostly initialization mismatch, not pure offset.
- Fix: temporary offset probe mode was used and then removed.
- Status: closed (probe tooling removed).

### 5) UI built for 480x480 only
- Symptom: layout/typography too large or clipped on 284x240 xingzhi panel.
- Root cause: hardcoded 480x480 constants and large-font assumptions in UI code.
- Fix: added xingzhi-specific responsive layout constants and smaller font mapping.
- Status: fixed.

### 6) Splash renderer hardcoded to 480x480
- Symptom: splash assets/layout not appropriate for 284x240 panel.
- Root cause: fixed CELL and canvas dimensions.
- Fix: splash cell/canvas now derived from panel size.
- Status: fixed.

### 7) Temporary boot-only debug mode
- Symptom addressed: needed a clean rendering baseline to separate panel issues from asset/layout issues.
- Fix: black screen + centered green "Booting up" screen (xingzhi only).
- Outcome: confirmed display path, centering, and basic rendering were sane.
- Status: removed after full UI path validation.

### 8) Flash/port instability during iteration
- Symptom: COM port busy/missing intermittently during upload attempts.
- Root cause: USB serial contention/device reconnect timing.
- Fix: detect active COM port before upload and retry with explicit port.
- Status: operational workaround.

## Validation Result
1. Full xingzhi UI runtime path restored.
2. Usage and Bluetooth screens verified on 284x240 layout.
3. Splash animation renders at panel-sized scaling.
4. Accent colors corrected with orange-tinted splash path.
5. Build + flash validation passed for `xingzhi_cube_183`.

## Notes
- Asset conversion (icon/logo/splash preprocessing) should only proceed after panel init + layout are stable.
- On xingzhi, oversized decorative icons are disabled pending a dedicated small-asset pack.
