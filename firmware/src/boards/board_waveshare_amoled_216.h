#pragma once
//
// Board: Waveshare ESP32-S3-Touch-AMOLED-2.16
// 480x480 round-square AMOLED (CO5300, QSPI), CST9220 touch, AXP2101 PMU,
// QMI8658 accelerometer. All peripherals share one I2C bus.
//
// This is the original Clawdmeter board.
//

// ---- Display ----
#define BOARD_NAME              "Waveshare AMOLED 2.16\""
#define BOARD_LCD_W             480
#define BOARD_LCD_H             480
#define BOARD_LCD_IS_AMOLED     1     // QSPI CO5300; brightness control via DCS
#define BOARD_LCD_IS_ROUND      1     // visible window has rounded corners
#define BOARD_FIXED_ROTATION    0     // auto-rotation overrides at runtime

// QSPI CO5300 pins
#define LCD_CS                  12
#define LCD_SCLK                38
#define LCD_SDIO0               4
#define LCD_SDIO1               5
#define LCD_SDIO2               6
#define LCD_SDIO3               7
#define LCD_RESET               2

// ---- I2C (shared by touch, PMU, IMU) ----
#define IIC_SDA                 15
#define IIC_SCL                 14

// ---- Touch (CST9220) ----
#define BOARD_HAS_TOUCH         1
// Unified SensorLib CST driver — auto-detects the actual chip at the
// configured address, so the same type works for CST92xx (Waveshare
// AMOLED) and CST816 (Touch-LCD-2).
#define BOARD_TOUCH_CLASS       TouchDrvCSTXXX
#define BOARD_TOUCH_ADDR        0x5A
#define TP_INT                  11
#define TP_RST                  2     // shared with LCD_RESET

// ---- PMU (AXP2101) ----
#define BOARD_HAS_PMU           1
#define BOARD_HAS_BATTERY_ADC   0     // ADC path not used when PMU present
#define BOARD_HAS_PWR_BUTTON    1     // AXP2101 PKEY drives the mid button
#define AXP2101_ADDR            0x34

// ---- IMU (QMI8658, auto-rotation) ----
#define BOARD_HAS_IMU           1

// ---- Physical buttons ----
// Two on-board side buttons + one PMU PKEY make the 3-button scheme. The
// PMU button is wired through power.cpp (HAS_PWR_BUTTON above) — only the
// two GPIO buttons are described here.
#define BOARD_BTN_COUNT         2
#define BOARD_BTN0_GPIO         0     // left — Space (voice-mode push-to-talk)
#define BOARD_BTN1_GPIO         18    // right — Shift+Tab (mode toggle)
#define BOARD_BTN_ACTIVE_LOW    1
