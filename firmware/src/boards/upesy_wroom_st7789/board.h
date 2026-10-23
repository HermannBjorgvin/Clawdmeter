#pragma once

// uPesy ESP32 WROOM DevKit + a bare 1.9" 170x320 ST7789 SPI module, wired by
// hand. Classic ESP32-D0WD (dual-core Xtensa, 4 MB flash, NO PSRAM), CH340
// USB-UART. No touch, no battery, no IMU: the BOOT button is the only input.
//
// Same SoC and no-PSRAM constraints as the CYD port (esp32_2432s028r); same
// panel shape as the T-Display S3 (170x320 with a 35 px column offset).

#define BOARD_NAME           "uPesy WROOM + ST7789 1.9\""

// ---- Display geometry ----
// The ST7789 GRAM is 240x320; this glass shows the middle 170 columns, so the
// native size is 170x320 with a 35 px column offset. Portrait only: 320x170
// landscape doesn't fit the shared layout's stacked cards (see CLAUDE.md).
#define LCD_NATIVE_WIDTH     170
#define LCD_NATIVE_HEIGHT    320
#define LCD_COL_OFFSET       35
#define LCD_ROW_OFFSET       0

#define LCD_WIDTH            170
#define LCD_HEIGHT           320
#define LCD_ROTATION         2     // portrait, upside down (how the reference unit is mounted)

// ---- Display SPI (ST7789, 4-wire) — the ESP32's native VSPI pins ----
#define LCD_SCLK             18
#define LCD_MOSI             23
#define LCD_CS               15    // strapping pin; fine as CS (idles high)
#define LCD_DC               2     // strapping pin; must not be pulled high at reset
#define LCD_RST              4
#define LCD_BL               32    // backlight, LEDC PWM (panel has no brightness cmd)
#define LCD_SPI_SPEED        40000000  // 40 MHz; jumper wires rarely hold 80

// ---- Buttons ----
// BOOT (GPIO 0) is the only user button. As on the CYD it takes the PWR role
// (power.cpp: short → brightness, hold and release → pair). No HID Space.
#define BTN_PWR_GPIO         0
#define BTN_PWR_LONG_MS      1500

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         0
#define BOARD_HAS_IMU              0
#define BOARD_HAS_BATTERY          0
#define BOARD_HAS_IO_EXPANDER      0
#define BOARD_HAS_SOUND            0
