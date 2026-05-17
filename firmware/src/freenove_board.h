#pragma once

// Freenove ESP32-S3 Display NonTouch minimal target.
#define LCD_WIDTH   240
#define LCD_HEIGHT  320

#define BOARD_HAS_TOUCH        0
#define BOARD_HAS_PMU          0
#define BOARD_HAS_IMU          0
#define BOARD_HAS_HID_BUTTONS  0

// TFT pins for FNK0104B NonTouch from Freenove's official TFT_eSPI setup.
#define TFT_CS      10
#define TFT_MOSI    11
#define TFT_SCLK    12
#define TFT_MISO    13
#define TFT_BL      45
#define TFT_DC      46
#define TFT_RST     -1
