#pragma once

// Waveshare ESP32-S3-Touch-LCD-2 -- 2" 240x320 portrait ST7789T3 IPS TFT over
// plain 4-wire SPI, CST816D capacitive touch, QMI8658C IMU (unused), battery
// ADC (no PMU: a standalone linear charger). SD card and OV3660 camera are
// wired but not used by Clawdmeter. ESP32-S3R8: 16 MB flash + 8 MB octal PSRAM.
// https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-2
//
// Pin map carried over from the PR author's bring-up on this kit.

#define BOARD_NAME           "Waveshare LCD 2"

// ---- Display geometry ----
#define LCD_WIDTH            240
#define LCD_HEIGHT           320

// ---- SPI display pins (ST7789T3) ----
// LCD_RST shares GPIO 0 with BOOT/strap: gfx->begin() pulses it low once at
// start-up. Afterwards GPIO 0 stays high, so BOOT can only act as a hardware
// reset and is not exposed as an input (input.cpp).
#define LCD_MOSI             38
#define LCD_SCLK             39
#define LCD_DC               42
#define LCD_CS               45
#define LCD_RST              0
#define LCD_BL               1     // backlight, LEDC PWM (TFT has no brightness cmd)

// ---- I2C bus (touch + IMU) ----
#define IIC_SDA              48
#define IIC_SCL              47

// ---- Touch (CST816D, SensorLib auto-detects the CST816 family) ----
#define TP_INT               46
#define TP_RST               -1    // not wired; the chip self-resets at POR
#define TOUCH_ADDR           0x15

// ---- Battery (ADC divider, no PMU) ----
// R19 = 200K / R20 = 100K: the pin sees VBAT / 3.
#define BAT_ADC_PIN          5
#define BAT_VOLT_DIVIDER     3.0f
// The charger status pin is not exposed, so "USB plugged in" (HWCDC) stands
// in for charging.
#define BAT_USB_CDC_DETECT   1

// ---- Buttons ----
// KEY1 is ESP_EN (hardware reset) and KEY2 is BOOT/GPIO 0, which doubles as
// LCD reset: neither is an application input. Navigation is touch-only, and
// pairing is the 5 s touch hold on the pairing hint (caps.has_pwr_button=false).

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         0
#define BOARD_HAS_IMU              0    // QMI8658C present (INT1 = GPIO 3), unused
#define BOARD_HAS_BATTERY          1
#define BOARD_HAS_IO_EXPANDER      0
