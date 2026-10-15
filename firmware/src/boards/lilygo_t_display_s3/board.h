#pragma once

// LILYGO T-Display S3 (Basic, no touch) -- 170x320 portrait ST7789 IPS TFT on
// an 8-bit i80 parallel bus. ESP32-S3R8: 16 MB flash + 8 MB octal PSRAM. No
// PMU, no IMU, no touch; battery voltage is read through an ADC divider.
// Two front buttons: BOOT (GPIO 0) and a user button (GPIO 14).
//
// Pin map carried over from the PR author's bring-up on this kit; the panel
// rail (GPIO 15) and backlight (GPIO 38) are plain GPIOs.

#define BOARD_NAME           "LILYGO T-Display S3"

// ---- Display geometry ----
#define LCD_WIDTH            170
#define LCD_HEIGHT           320

// ---- ST7789 i80 parallel bus ----
#define LCD_DC               7
#define LCD_CS               6
#define LCD_WR               8
#define LCD_RD               9
#define LCD_RST              5
#define LCD_BL               38    // backlight, LEDC PWM (TFT has no brightness cmd)
#define LCD_POWER_ON         15    // panel power rail enable (HIGH = on)

#define LCD_D0               39
#define LCD_D1               40
#define LCD_D2               41
#define LCD_D3               42
#define LCD_D4               45
#define LCD_D5               46
#define LCD_D6               47
#define LCD_D7               48

// The ST7789's RAM is 240 wide but only 170 columns reach the glass: the
// visible window starts at column 35.
#define LCD_COL_OFFSET       35
#define LCD_ROW_OFFSET       0

// ---- Battery (ADC divider, no PMU) ----
#define BAT_ADC_PIN          4
#define BAT_VOLT_DIVIDER     2.0f  // 100k / 100k

// ---- Buttons (active-LOW GPIOs, internal pull-ups) ----
// BOOT (GPIO 0) is a strap pin: do not hold it through power-up.
#define BTN_BACK_GPIO        0     // BOOT -- primary, Space (PTT)
#define BTN_PWR_GPIO         14    // PWR role: brightness, hold-to-pair

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         0
#define BOARD_HAS_IMU              0
#define BOARD_HAS_BATTERY          1
#define BOARD_HAS_IO_EXPANDER      0
