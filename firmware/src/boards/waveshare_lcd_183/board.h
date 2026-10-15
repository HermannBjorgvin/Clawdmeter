#pragma once

// Waveshare ESP32-S3-Touch-LCD-1.83 — 240x284 IPS LCD (ST7789P over 4-wire SPI).
// Shares the AMOLED-1.8's I2C peripheral set (AXP2101 PMU, QMI8658 IMU, CST816
// touch) on the same SDA/SCL pins, but the display is an SPI TFT — not a QSPI
// AMOLED — and there is no XCA9554 IO expander: the LCD/touch reset lines are
// plain GPIOs and the backlight is a GPIO-driven LED string. Pins taken from
// Waveshare's official demo (waveshareteam/ESP32-S3-Touch-LCD-1.83, pin_config.h).

#define BOARD_NAME           "Waveshare LCD 1.83"

// ---- Display geometry (portrait, fixed 0°) ----
#define LCD_WIDTH            240
#define LCD_HEIGHT           284
// ST7789 GRAM is 240x320. Waveshare's demo uses row offset 20, but on this
// panel the visible glass maps to GRAM rows [0..283] — a row offset of 20
// leaves the top ~20 rows unwritten (power-on garbage) and clips the bottom.
// 0 aligns the 284-row window to the top of GRAM. (col offset stays 0.)
#define LCD_ROW_OFFSET       0

// ---- Display pins (ST7789P, 4-wire SPI) ----
#define LCD_DC               4
#define LCD_CS               5
#define LCD_SCK              6
#define LCD_MOSI             7
#define LCD_RST              38    // real GPIO (no IO expander on this board)
#define LCD_BL               40    // backlight enable, active HIGH (PWM-dimmed)

// ---- I2C bus (touch + PMU + IMU share one bus) ----
#define IIC_SDA              15
#define IIC_SCL              14

// ---- Touch (CST816D, FocalTech-style data layout @ regs 0x02..0x06) ----
#define TP_INT               13
#define TP_RST               39    // real GPIO; released in board_init()
#define CST816_ADDR          0x15

// ---- PMU ----
#define AXP2101_ADDR         0x34

// ---- Buttons ----
#define BTN_BACK_GPIO        0     // BOOT — primary, Space (PTT)
// PWR comes via the AXP2101 PKEY IRQ (see power.cpp); no secondary GPIO button.

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         0
#define BOARD_HAS_IMU              1
#define BOARD_HAS_BATTERY          1
#define BOARD_HAS_IO_EXPANDER      0
