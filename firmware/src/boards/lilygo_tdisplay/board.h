#pragma once

// LilyGO TTGO T-Display (classic ESP32) — 1.14" ST7789 135x240 over plain
// 4-wire SPI. A vendor board, unlike the DIY esp32_devkit_st7789 port, but the
// same SoC family and the same panel controller, so it reuses that port's
// shape: classic ESP32, no PSRAM, no native USB, PWM backlight.
//
// Everything below was confirmed on the actual board with a throwaway probe
// (chip ID, PSRAM size, panel init, button GPIOs) rather than taken from a
// wiki — the pin map matches the LILYGO_T_DISPLAY block in Arduino_GFX's
// examples/PDQgraphicstest/Arduino_GFX_dev_device.h.
//
// Measured: ESP32-D0WDQ6 rev 1.0, 240 MHz, 16 MB flash, PSRAM 0 bytes.
//
// The smallest panel in the tree by a wide margin — 135 px across, against 240
// on the next smallest. The splash stage is min(W,H)/60 = 2 px per cell.

#define BOARD_NAME           "LilyGO T-Display"

// ---- Display geometry ----
// Landscape. Unlike the CO5300 AMOLEDs — whose MADCTL can only flip axes, so
// they pay for rotation with a CPU pixel remap — the ST7789 exchanges rows and
// columns in hardware. Rotation here costs one register write and nothing per
// frame, so there is no rotated-strip buffer and display_hal_tick stays empty.
//
// Two sizes, deliberately separate:
//   LCD_PANEL_W/H — the panel's native portrait geometry, what the
//                   Arduino_ST7789 constructor wants.
//   LCD_WIDTH/HEIGHT — what LVGL and the UI see after rotation, which is what
//                   caps.cpp publishes.
// Mixing them up gives a display that draws into a 135-wide window on a
// 240-wide screen.
#define LCD_PANEL_W          135
#define LCD_PANEL_H          240
#define LCD_ROTATION         1     // 1 = landscape; use 3 for the same landscape upside down

#define LCD_WIDTH            240
#define LCD_HEIGHT           135

// The ST7789's GRAM is 240x320; this panel exposes a 135x240 window inside it,
// hence the offsets. Probe-verified in portrait: with 52/40 the image sits
// centered with no garbage strip at any edge. All four are passed as-is —
// Arduino_TFT::setRotation picks the right pair per rotation (for rotation 1 it
// takes ROW_OFFSET1 as x and COL_OFFSET2 as y), so they never need swapping by
// hand.
#define LCD_COL_OFFSET1      52
#define LCD_ROW_OFFSET1      40
#define LCD_COL_OFFSET2      53
#define LCD_ROW_OFFSET2      40

// ---- Display pins (4-wire SPI) ----
#define LCD_SCLK             18
#define LCD_MOSI             19
#define LCD_CS               5
#define LCD_DC               16
#define LCD_RST              23
#define LCD_BL               4     // backlight, LEDC PWM (ST7789 has no brightness cmd)

// ---- Buttons ----
// Only two are readable by software. The board's third button is RST, wired
// straight to the chip's EN line — it resets the ESP32 and never reaches a
// GPIO, exactly like the T-Display-S3. Both readable ones are active LOW.
//
// With no touch controller, the PWR role has to go on a physical button or the
// usage screen is unreachable (see caps.has_touch), so GPIO35 takes it and
// this board has no secondary/HID button.
#define BTN_BACK_GPIO        0     // BOOT — primary, Space (PTT)
#define BTN_PWR_GPIO         35    // PWR role — cycle screens / hold-to-pair

// GPIO 35 is one of the ESP32's input-only pins (34..39): no internal pull-up
// is available. The board provides an external one — probe-confirmed, it idles
// HIGH as a plain INPUT — so power.cpp must use INPUT, not INPUT_PULLUP.
#define BTN_PWR_INPUT_ONLY   1

// ---- Battery ----
// VBAT through a 2:1 divider on GPIO34 (also input-only). Reads ~4.2 V on USB
// whether or not a cell is plugged in — the divider hangs off the charger
// output, so "no battery" is indistinguishable from "full battery" here.
#define BAT_ADC_PIN          34
#define BAT_VOLT_DIVIDER     2.0f

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         0
#define BOARD_HAS_IMU              0
#define BOARD_HAS_BATTERY          1
#define BOARD_HAS_IO_EXPANDER      0
#define BOARD_HAS_SOUND            0
#define BOARD_HAS_TOUCH            0   // display-only board — PWR toggles screens
