#pragma once

// Waveshare ESP32-C6-LCD-1.47 / -M (non-touch, ESP32-C6FH4).
// Pin map and 34-column panel offset: Waveshare Arduino example.
#define BOARD_NAME "Waveshare LCD 1.47 (C6, non-touch)"
#define LCD_WIDTH 320
#define LCD_HEIGHT 172
#define LCD_PANEL_W 172
#define LCD_PANEL_H 320
#define LCD_COL_OFFSET 34
#define LCD_ROTATION 1
#define LCD_MOSI 6
#define LCD_SCLK 7
#define LCD_CS 14
#define LCD_DC 15
#define LCD_RST 21
#define LCD_BL 22
#define BTN_PWR_GPIO 9
#define BOARD_HAS_ROTATION 0
#define BOARD_HAS_BATTERY 0
#define BOARD_HAS_IMU 0
