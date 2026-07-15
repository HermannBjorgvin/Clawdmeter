#pragma once

// SpotPear ESP32-S3 1.54" LCD (N16R8) — an UNOFFICIAL community port.
//
// This board is NOT one of the four upstream-supported panels. It differs
// from them in one big way: the display is a plain 4-wire SPI ST7789
// (240x240 IPS), NOT a QSPI AMOLED. So display.cpp here uses Arduino_HWSPI +
// Arduino_ST7789 instead of Arduino_ESP32QSPI + an AMOLED driver.
//
// MCU side matches the supported S3 boards: ESP32-S3 + 16MB flash + 8MB OPI
// PSRAM (the "N16R8" suffix), so BOARD_HAS_PSRAM and qio_opi memory apply.
//
// Pins verified from the xiaozhi-esp32 board config `sp-esp32-s3-1.54-muma`
// (github.com/78/xiaozhi-esp32) — this board is the "Spotpear ESP32-S3-LCD-
// 1.54-MUMA". The touch pins (SDA=11, SCL=7) were independently confirmed by
// an on-hardware I2C scan. NOTE: this board does NOT follow the Waveshare
// ESP32-S3-Touch-LCD-1.54 pinmap despite SpotPear's wiki linking to it.

#define BOARD_NAME           "SpotPear ESP32-S3 1.54 MUMA"

// ---- Display geometry ----
#define LCD_WIDTH            240
#define LCD_HEIGHT           240

// ---- ST7789 4-wire SPI pins ----
#define LCD_DC               47
#define LCD_CS               5
#define LCD_SCK              4
#define LCD_MOSI             2
#define LCD_RST              38
#define LCD_SPI_HZ           40000000   // 40 MHz per board config

// Backlight is PWM on GPIO 42 and **active-LOW** (DISPLAY_BACKLIGHT_OUTPUT_
// INVERT=true): duty is inverted so level 255 = fully on.
#define LCD_BL               42
#define LCD_BL_INVERT        1

// True 240x240 panel — no window offsets, no mirror/swap (per board config).
#define LCD_COL_OFFSET       0
#define LCD_ROW_OFFSET       0

// ---- I2C bus (touch) ----  SDA=11, SCL=7 (confirmed by I2C scan)
#define IIC_SDA              11
#define IIC_SCL              7

// ---- Touch: CST816 @ 0x15 (FocalTech-style register layout) ----
#define TP_INT               12
#define TP_RST               6
#define TP_ADDR              0x15

// ---- Buttons ----
#define BTN_BACK_GPIO        0     // BOOT — primary, Space (push-to-talk)
// The board also has a "PLUS" button; wire it as a secondary later if desired
// (set its GPIO here and flip BOARD_HAS_SECONDARY_BUTTON to 1).

// ---- Capability flags ---- (keep in sync with caps.cpp)
#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         0   // ST7789 rotates natively; auto-rotate off
#define BOARD_HAS_IMU              0   // QMI8658 present but unused for now
#define BOARD_HAS_BATTERY          0   // simple charger + ADC; no PMU. Stubbed.
#define BOARD_HAS_IO_EXPANDER      0
#define BOARD_HAS_SOUND            0   // no buzzer/codec — sound.cpp is a no-op
