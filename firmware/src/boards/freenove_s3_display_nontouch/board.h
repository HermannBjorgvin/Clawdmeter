#pragma once

// Freenove ESP32-S3 Display, non-touch version (FNK0104A) - 2.8" 240x320
// portrait IPS TFT, ILI9341 over plain 4-wire SPI. ESP32-S3R8: 16 MB flash +
// 8 MB octal PSRAM. No touch, no PMU, no IMU; the panel has no brightness
// command, so backlight is LEDC PWM on TFT_BL.
//
// Pin map from Freenove's official TFT_eSPI setup, hardware-tested by the
// original port author (display and BLE verified on a real board).

#define BOARD_NAME           "Freenove S3 Display"

// ---- Display geometry (portrait) ----
#define LCD_WIDTH            240
#define LCD_HEIGHT           320

// ---- SPI display pins (ILI9341) ----
#define TFT_CS               10
#define TFT_MOSI             11
#define TFT_SCLK             12
#define TFT_MISO             13
#define TFT_BL               45    // backlight, LEDC PWM
#define TFT_DC               46
#define TFT_RST              -1    // no reset line; software reset

// ---- Buttons ----
#define BTN_PWR_GPIO         0     // BOOT - PWR role: brightness, hold-to-pair

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         0
#define BOARD_HAS_IMU              0
#define BOARD_HAS_BATTERY          0
#define BOARD_HAS_IO_EXPANDER      0
