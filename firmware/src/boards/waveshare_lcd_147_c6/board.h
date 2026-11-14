#pragma once

// Waveshare ESP32-C6-Touch-LCD-1.47 — 172x320 JD9853 IPS TFT kit, driven here
// in LANDSCAPE (320x172). The C6 sibling of waveshare_lcd_147: the same JD9853
// panel over plain 4-wire SPI (register-compatible enough with the ST7789
// command set once the vendor init sequence has been pushed) and the same
// AXS5106L capacitive touch, plus a QMI8658 IMU, on a completely different
// GPIO map — the C6 only has GPIO0-30 (no PSRAM, single-core RISC-V, BLE 5
// only), so none of the S3 kit's pin numbers carry over.
//
// Pin map from Waveshare's docs (docs.waveshare.com/ESP32-C6-Touch-LCD-1.47),
// confirmed on hardware. One correction to the docs: BOOT is GPIO9, not the
// GPIO8 they list (GPIO9 is the C6's BOOT strapping pin, as on the AMOLED-1.8
// C6).
//
// Battery: an ETA6098 standalone charger (no digital interface, no CHG/STAT
// pin broken out) and a VBAT divider on GPIO0 (BAT_ADC: R21 200K to VBAT, R22
// 100K to GND, VBAT = VADC × 3). BOARD_HAS_BATTERY is 0 by default: with no
// cell on VBAT the reading is whatever the charger leaves on the pin, and on
// the tested unit that is ~4.2 V, a confidently wrong "full battery". If a
// cell is wired to VBAT, set it to 1 and rebuild. See this board's CLAUDE.md.

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
// (a panel-controller property, the same on the S3 kit).
// Arduino_TFT::setRotation maps this offset onto the correct axis per rotation.
#define LCD_COL_OFFSET       34

// ---- SPI display pins (JD9853, 4-wire SPI) ----
#define LCD_CS               14
#define LCD_SCLK             1
#define LCD_MOSI             2
#define LCD_DC               15
#define LCD_RST              22
#define LCD_BL               23    // backlight, LEDC PWM (panel has no brightness cmd)

// ---- I2C bus (touch + IMU) ----
#define IIC_SDA              18
#define IIC_SCL              19

// ---- Touch (AXS5106L, minimal inline I2C reader) ----
#define TP_INT               21
#define TP_RST               20
#define AXS5106L_ADDR        0x63

// ---- IMU (QMI8658, populated but rotation disabled — see imu.cpp) ----
#define QMI8658_ADDR         0x6B

// ---- Battery (ADC divider, no I2C-readable PMU) ----
// VBAT -> R21(200K)/R22(100K) divider -> GPIO0 (BAT_ADC). VBAT = VADC * 3.
#define BAT_ADC_PIN          0
#define BAT_VOLT_DIVIDER     3.0f

// ---- Buttons ----
// The kit ships exactly two keys: BOOT (GPIO9) and RESET (tied to EN, not
// readable). So there is only ONE usable user button, and it is given the PWR
// role — brightness / the hold-3s-release pairing gesture — because without
// it the device could never be paired at all. No HID push-to-talk or
// mode-toggle button exists here; input.cpp reports no buttons and
// BoardCaps.button_count is 0.
#define BTN_PWR_GPIO         9     // BOOT — PWR role

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         0    // panel is rotated once at init, not at runtime
#define BOARD_HAS_IMU              1    // QMI8658: bus health only, no rotation
#define BOARD_HAS_BATTERY          0    // off by default, see the battery note above
#define BOARD_HAS_IO_EXPANDER      0
#define BOARD_HAS_SOUND            0    // no codec, no buzzer
