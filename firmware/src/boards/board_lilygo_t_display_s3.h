#pragma once
//
// Board: LILYGO T-Display S3 (Basic, no touch)
// 170x320 ST7789 IPS LCD via 8-bit i80 parallel, two front buttons (BOOT
// on GPIO0, user button on GPIO14), battery voltage on GPIO4 ADC. No PMU,
// no IMU, no touch.
//
// ESP32-S3R8, 16 MB flash, 8 MB PSRAM.
//

// ---- Display ----
#define BOARD_NAME              "LILYGO T-Display S3"
#define BOARD_LCD_W             170
#define BOARD_LCD_H             320
// ST7789 parallel, not AMOLED — no DCS brightness command, backlight is GPIO.
#define BOARD_LCD_IS_AMOLED     0
#define BOARD_LCD_IS_ROUND      0
// Portrait by default. If you prefer landscape, set to 1 or 3 and re-flash —
// LVGL and the GFX driver pick it up at boot. There is no IMU on this board
// so rotation is fixed.
#ifndef BOARD_FIXED_ROTATION
#define BOARD_FIXED_ROTATION    0
#endif

// ST7789 control pins
#define LCD_DC                  7
#define LCD_CS                  6
#define LCD_WR                  8
#define LCD_RD                  9
#define LCD_RESET               5
#define LCD_BL                  38     // backlight
#define LCD_POWER_ON            15     // panel power rail enable

// 8-bit parallel data pins
#define LCD_D0                  39
#define LCD_D1                  40
#define LCD_D2                  41
#define LCD_D3                  42
#define LCD_D4                  45
#define LCD_D5                  46
#define LCD_D6                  47
#define LCD_D7                  48

// ST7789 internal RAM is 240x320 but the visible panel is 170 columns —
// the column window starts at x=35.
#define LCD_COL_OFFSET          35
#define LCD_ROW_OFFSET          0

// ---- I2C ----
// Not used by the Basic variant — leave the symbols defined so files that
// reference them don't break, but nothing initializes the bus.
#define IIC_SDA                 -1
#define IIC_SCL                 -1

// ---- Touch ----
#define BOARD_HAS_TOUCH         0     // Basic variant has no touch controller

// ---- PMU ----
#define BOARD_HAS_PMU           0     // no AXP2101 on this board
#define BOARD_HAS_PWR_BUTTON    0
#define BOARD_HAS_BATTERY_ADC   1     // approximate battery voltage from ADC

// Battery ADC (un-calibrated; logs the raw value at boot so the divider can
// be derived empirically). Holding-stock cells like a 3.7 V LiPo go through
// a 2:1 voltage divider on this board.
#define BAT_ADC_GPIO            4
#define BAT_ADC_MAX             4095        // 12-bit
#define BAT_ADC_REF_VOLTAGE     3.3f        // ESP32-S3 VDDA reference
#define BAT_ADC_DIVIDER_RATIO   2.0f        // empirical: 100k/100k
#define BAT_ADC_CORRECTION_FACTOR 1.0f      // tweak after calibration

// ---- IMU ----
#define BOARD_HAS_IMU           0

// ---- Physical buttons ----
// BOOT/IO0 is a strap pin — DO NOT hold it during power-up or the chip
// enters download mode. After boot it can be used as a normal input.
// IO14 is a plain GPIO. Both are wired to GND with internal pull-ups.
#define BOARD_BTN_COUNT         2
#define BOARD_BTN0_GPIO         0     // BOOT — short=back/splash, long=Space hold
#define BOARD_BTN1_GPIO         14    // IO14 — short=cycle screen, long=Shift+Tab
#define BOARD_BTN_ACTIVE_LOW    1
