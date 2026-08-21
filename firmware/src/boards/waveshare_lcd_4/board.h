#pragma once

// Waveshare ESP32-S3-Touch-LCD-4 — 4" square RGB TFT kit.
// 480x480 ST7701 (RGB parallel + SW SPI init) + GT911 touch + TCA9554-style
// IO expander at 0x24 (display power / backlight). No AXP2101, no IMU.
// Module is ESP32-S3R8 (8 MB OPI PSRAM, 16 MB flash).
//
// Pin map matches the hardware-tested lcd4 fork and Waveshare's wiki
// (RGB data pins are 0-based R0..R4 / G0..G5 / B0..B4 here; the wiki labels
// them R1..R5 / B1..B5). Later board revs share a single I2C bus (SDA=15,
// SCL=7) for both the expander and GT911 — GPIO 8/9 are RGB data and cannot
// be used as a second I2C once the panel is running.

#define BOARD_NAME           "Waveshare LCD 4"

// ---- Display geometry ----
#define LCD_WIDTH            480
#define LCD_HEIGHT           480

// ---- RGB panel pins (ST7701) ----
#define LCD_DE               40
#define LCD_VSYNC            39
#define LCD_HSYNC            38
#define LCD_PCLK             41
#define LCD_R0               46
#define LCD_R1                3
#define LCD_R2                8
#define LCD_R3               18
#define LCD_R4               17
#define LCD_G0               14
#define LCD_G1               13
#define LCD_G2               12
#define LCD_G3               11
#define LCD_G4               10
#define LCD_G5                9
#define LCD_B0                5
#define LCD_B1               45
#define LCD_B2               48
#define LCD_B3               47
#define LCD_B4               21

// ---- Software SPI for ST7701 init commands ----
#define LCD_SPI_CS           42
#define LCD_SPI_SCK           2
#define LCD_SPI_MOSI          1

// ---- I2C bus (GT911 + IO expander) ----
#define IIC_SDA              15
#define IIC_SCL               7

// ---- Touch (GT911, polled — INT is GPIO 16 on the wiki but unused here) ----
#define TP_INT               16

// ---- IO expander (TCA9554 / CH32V003-as-expander) ----
// Controls display power rails and backlight (EXIO2). Must be programmed
// before gfx->begin() or the panel stays dark.
#define IO_EXPANDER_ADDR     0x24
#define IO_EXPANDER_ADDR_ALT 0x20
#define IOX_PIN_BACKLIGHT    2     // Waveshare wiki: expander pin 2 = backlight

// ---- Buttons ----
#define BTN_BACK_GPIO        0     // BOOT — primary, Space (PTT)
// KEY/PWR is wired to EN/RST (hardware reset), not a GPIO. No secondary
// button — GPIO 18 is display R3.

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         0
#define BOARD_HAS_IMU              0
#define BOARD_HAS_BATTERY          0
#define BOARD_HAS_IO_EXPANDER      1
#define BOARD_HAS_SOUND            0
