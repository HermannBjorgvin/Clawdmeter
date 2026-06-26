#pragma once

// Waveshare ESP32-S3-Touch-LCD-1.83 — portrait IPS LCD kit.
// 240x284 ST7789P (SPI) + CST816 touch + AXP2101 PMU + QMI8658 IMU.
// No IO expander: LCD_RST / TP_RST are direct GPIOs. Backlight on GPIO 40.
// IMU is populated (initialized for I2C bus health) but rotation is disabled —
// the panel mounts in a fixed orientation in the kit's enclosure.

#define BOARD_NAME           "Waveshare LCD 1.83"

// ---- Display geometry (portrait) ----
#define LCD_WIDTH            240
#define LCD_HEIGHT           284
// ST7789 GRAM is 240x320. Waveshare's demo passes row offset 20, but on this
// panel the visible glass maps to GRAM rows [0..283]: an offset of 20 leaves
// the top ~20 rows unwritten (power-on garbage — the static band) and clips the
// bottom. 0 aligns the 284-row window to the top of GRAM. (col offset stays 0.)
#define LCD_ROW_OFFSET       0

// ---- SPI display pins (ST7789P) ----
#define LCD_DC               4
#define LCD_CS               5
#define LCD_SCK              6
#define LCD_MOSI             7
#define LCD_RST              38     // direct GPIO (handled by the GFX driver)
#define LCD_BL               40     // backlight — PWM for brightness

// ---- I2C bus (touch + PMU + IMU all share one bus) ----
#define IIC_SDA              15
#define IIC_SCL              14

// ---- Touch (CST816 — same inline FocalTech-style reader as AMOLED-1.8) ----
#define TP_INT               13
#define TP_RST               39     // direct GPIO, active LOW
#define CST816_ADDR          0x15

// ---- PMU ----
#define AXP2101_ADDR         0x34

// ---- Buttons ----
#define BTN_BACK_GPIO        0      // BOOT — primary, Space (PTT)
// PWR comes via the AXP2101 PKEY IRQ (see power.cpp); no secondary button.

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         0
#define BOARD_HAS_IMU              1    // present + initialized, rotation off
#define BOARD_HAS_BATTERY          1
#define BOARD_HAS_IO_EXPANDER      0
