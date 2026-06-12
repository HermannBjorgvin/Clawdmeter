#pragma once

// LilyGo T4-S3 — 2.41" RM690B0 AMOLED, 450x600 native (portrait).
// We use landscape orientation: logical 600x450.
// All peripherals are accessed through the global `amoled` LilyGo_AMOLED
// instance initialised in board_init.cpp.

#include <LilyGo_AMOLED.h>

// Single board-wide peripheral instance. Defined in board_init.cpp;
// display.cpp, touch.cpp, and power.cpp reference it via this extern.
extern LilyGo_AMOLED amoled;

#define BOARD_NAME  "LilyGo T4-S3"

// Logical dimensions in landscape orientation.
#define LCD_WIDTH   600
#define LCD_HEIGHT  450

// ---- QSPI display pins (RM690B0) ----
#define LCD_D0      14
#define LCD_D1      10
#define LCD_D2      16
#define LCD_D3      12
#define LCD_SCLK    15
#define LCD_CS      11
#define LCD_RESET   13
#define LCD_TE      18

// ---- Shared I2C bus ----
#define IIC_SDA     6
#define IIC_SCL     7

// ---- PMIC enable (must be HIGH before display init) ----
#define PMIC_EN_PIN 9

// ---- Physical button ----
#define BTN_BACK_GPIO  0   // BOOT — INPUT_BTN_PRIMARY (PTT / Space)

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON  0
#define BOARD_HAS_ROTATION          0   // fixed landscape, no IMU
#define BOARD_HAS_IMU               0
#define BOARD_HAS_BATTERY           0   // SY6970 charger; % not readable without calibration
#define BOARD_HAS_TOUCH             0   // no capacitive touch controller fitted
