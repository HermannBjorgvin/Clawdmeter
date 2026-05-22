#pragma once

// Xingzhi Cube 1.83" TFT WiFi (2 mic variant) — ESP32-S3-N16R8, landscape
// 284x240 ST7789 over SPI, no touch, no PMU IC, no IMU.
// Three physical buttons (BOOT + VOL_UP + VOL_DOWN). Battery is a bare
// Li-Po with a charge-status pin (CHRG) on GPIO38 and a voltage-divider
// on GPIO17 (ADC2_CH6). GPIO21 latches power (drive HIGH to stay on).
//
// Pin map taken from the xiaozhi-esp32 board config:
//   https://github.com/RealDeco/xiaozhi-esp32_vietnam/blob/Danish-and-English-Radio/main/boards/xingzhi-cube-1.83tft-wifi/config.h

#define BOARD_NAME           "Xingzhi Cube 1.83"

// ---- Display geometry (landscape, native orientation) ----
#define LCD_WIDTH            284
#define LCD_HEIGHT           240

// ---- ST7789 SPI display pins ----
#define LCD_MOSI             10
#define LCD_SCLK             9
#define LCD_DC               8
#define LCD_CS               14
#define LCD_RESET            18
#define LCD_BL               13

// ---- Buttons ----
// BOOT is the primary (Space PTT). VOL_DOWN is the secondary (Shift+Tab
// mode toggle). VOL_UP doubles as the PWR/cycle-screens button — handled
// inside power.cpp via a simple GPIO edge detector.
#define BTN_BACK_GPIO        0     // BOOT — primary
#define BTN_FWD_GPIO         40    // VOL_DOWN — secondary
#define BTN_PWR_GPIO         39    // VOL_UP — used as PWR/cycle screens

// ---- Battery / charging ----
// Standalone charger IC; CHRG goes HIGH while charging. Battery voltage
// is read through a ~2:1 divider on ADC2_CH6 (GPIO17). Calibration table
// from xiaozhi config: raw 1970 → 0 %, 2430 → 100 %.
#define BAT_ADC_GPIO         17
#define BAT_CHRG_GPIO        38
#define BAT_ADC_RAW_EMPTY    1970
#define BAT_ADC_RAW_FULL     2430

// Drive HIGH in board_init() to latch the power-hold circuit on.
#define PWR_LATCH_GPIO       21

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 1
#define BOARD_HAS_ROTATION         0
#define BOARD_HAS_IMU              0
#define BOARD_HAS_BATTERY          1
#define BOARD_HAS_IO_EXPANDER      0
