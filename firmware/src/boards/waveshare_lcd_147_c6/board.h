#pragma once

// Waveshare ESP32-C6-Touch-LCD-1.47 — 172x320 JD9853 IPS TFT kit, driven here
// in LANDSCAPE (320x172), the C6 sibling of the (still-unmerged, PR #148)
// ESP32-S3-Touch-LCD-1.47 port. Same panel family (JD9853 over plain 4-wire
// SPI, register-compatible enough with the ST7789 command set once the
// vendor init sequence has been pushed) and same AXS5106L capacitive touch,
// but a completely different GPIO map — the C6 only has GPIO0-30 (no PSRAM,
// single-core RISC-V, BLE 5 only), so none of the S3 sibling's pin numbers
// carry over.
//
// Pin map from Waveshare's official docs (docs.waveshare.com/ESP32-C6-Touch-
// LCD-1.47), confirmed end-to-end on hardware (display, touch, BOOT/PWR
// button, pairing gesture, battery ADC all working). One correction to the
// docs: BOOT is GPIO9 here, not the GPIO8 they list — GPIO9 is what actually
// works on hardware, and matches the sibling AMOLED-1.8 (C6) port's BOOT pin.
//
// No JST/PH battery connector on this kit, but there IS a battery charge
// circuit: an ETA6098 (standalone switching Li-ion charger, no digital
// interface — pure analog/pin-strapped, so no I2C fuel-gauge shortcut) that
// works with the ME6217C33M5G to provide 3.3V, plus a VBAT ADC divider
// (network name BAT_ADC) on GPIO0 — R21 (200K) pulls up to VBAT, R22 (100K)
// pulls down to GND, giving VBAT = VADC × 3.
//
// BOARD_HAS_BATTERY is nonetheless 0 by default: with no battery attached,
// the ETA6098 free-runs its output toward the ~4.2V float/regulation target
// with nothing there to load it down (confirmed on hardware: VBAT reads
// 4.2V to GND with nothing plugged in) — indistinguishable from a genuinely
// full battery on a single voltage read, since it's the charger's normal
// unloaded behavior, not measurement noise. This kit has no battery
// connector, so running with no battery is the common case; showing a
// confidently wrong "full battery" icon by default would be worse than
// showing none. If you've wired a battery to VBAT, flip BOARD_HAS_BATTERY
// to 1 and recompile.

#define BOARD_NAME           "Waveshare LCD 1.47 (C6)"

// ---- Display geometry ----
// Panel is physically 172x320 portrait; the firmware runs it rotated so the
// UI gets a 320x172 landscape canvas. LCD_WIDTH/HEIGHT are post-rotation
// (that's what BoardCaps and the UI layout consume).
#define LCD_WIDTH            320
#define LCD_HEIGHT           172
#define LCD_PANEL_W          172   // native, pre-rotation
#define LCD_PANEL_H          320
#define LCD_ROTATION         1     // 1 = landscape
// The 172-wide active area starts at column 34 of the JD9853's 240-wide GRAM
// (same offset as the S3 sibling — a panel-controller property, not
// SoC-specific). Arduino_TFT::setRotation maps this onto the right axis.
#define LCD_COL_OFFSET       34

// ---- SPI display pins (JD9853, 4-wire SPI) — confirmed on hardware ----
#define LCD_CS               14
#define LCD_SCLK             1
#define LCD_MOSI             2
#define LCD_DC               15
#define LCD_RST              22
#define LCD_BL               23    // backlight, LEDC PWM (panel has no brightness cmd)

// ---- I2C bus (touch + IMU) — confirmed on hardware ----
#define IIC_SDA              18
#define IIC_SCL              19

// ---- Touch (AXS5106L, minimal inline I2C reader — same as the S3 sibling) ----
#define TP_INT               21
#define TP_RST               20
#define AXS5106L_ADDR        0x63

// ---- IMU (QMI8658, populated but rotation disabled — see imu.cpp) ----
#define QMI8658_ADDR          0x6B

// ---- Battery (ADC divider, no I2C-readable fuel gauge) ----
// VBAT -> R21(200K)/R22(100K) divider -> GPIO0 (network name BAT_ADC per
// Waveshare's documentation). VBAT = VADC * 3.
#define BAT_ADC_PIN            0
#define BAT_VOLT_DIVIDER       3.0f

// ---- Buttons ----
// Exactly one usable key on this kit (BOOT), given the PWR role — screens /
// brightness / the hold-3s-release pairing gesture — same reasoning as the
// S3 1.47: without it the device could never be paired. No HID push-to-talk
// or mode-toggle button exists here; input.cpp reports no buttons and
// BoardCaps.button_count is 0. Screen switching is also available by
// tapping the touchscreen.
#define BTN_PWR_GPIO         9     // BOOT — PWR role

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         0    // panel is rotated once at init, not at runtime
#define BOARD_HAS_IMU              1    // QMI8658 confirmed present; bus health only, no rotation
#define BOARD_HAS_BATTERY          0    // ADC/divider code is correct, but off by default — see comment above
#define BOARD_HAS_IO_EXPANDER      0
#define BOARD_HAS_SOUND            0    // no codec, no buzzer
