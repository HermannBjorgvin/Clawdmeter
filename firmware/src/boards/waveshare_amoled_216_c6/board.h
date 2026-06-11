#pragma once

// Waveshare ESP32-C6-Touch-AMOLED-2.16.
//
// Despite the matching "2.16" model number, this board is hardware-
// distinct from the S3 AMOLED-2.16: it shares the CO5300 panel controller
// but uses a CST9217 touch controller and the C6 SoC's own GPIO map. There
// is no PSRAM. AXP2101 PMU and QMI8658 IMU carry over.
//
// Pin assignments verified against the official Waveshare XiaoZhi BSP at
// waveshareteam/ESP32-C6-Touch-AMOLED-2.16 (XiaoZhi config.h) and the
// ESP-IDF example user_config.h files in the same repo.

#define BOARD_NAME           "Waveshare AMOLED 2.16 (C6)"

#define LCD_WIDTH            480
#define LCD_HEIGHT           480

// ---- QSPI display pins (CO5300) ----
#define LCD_CS               15
#define LCD_SCLK             0
#define LCD_SDIO0            1
#define LCD_SDIO1            2
#define LCD_SDIO2            3
#define LCD_SDIO3            4
// LCD reset is not wired to a MCU GPIO on this board — the CO5300 relies
// on its internal power-on reset. The Arduino_GFX driver gets
// GFX_NOT_DEFINED for reset.

// ---- I2C bus (touch + PMU + IMU all share one bus) ----
#define IIC_SDA              8
#define IIC_SCL              7

// ---- Touch (CST9217 via TouchDrvCST92xx library) ----
// CST9217 is register-compatible with CST9220 in the relevant subset; the
// SensorLib CST92xx driver works against both. I2C address is the same.
#define TP_INT               5
#define TP_RST               11
#define CST9220_ADDR         0x5A

// ---- PMU ----
#define AXP2101_ADDR         0x34

// ---- Buttons ----
// Three side-mounted buttons:
//   BOOT (primary) — GPIO 9, sends Space (PTT) over BLE HID
//   PWR  (cycle screens) — AXP2101 PKEY IRQ, handled in power.cpp
//   KEY  (secondary) — GPIO 10, sends Shift+Tab (mode toggle) over BLE HID
// KEY GPIO isn't documented by Waveshare; identified empirically by
// scanning unused GPIOs at boot (see git history of board_init.cpp).
#define BTN_BACK_GPIO        9
#define BTN_FWD_GPIO         10

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 1
#define BOARD_HAS_ROTATION         0    // C6 has no PSRAM headroom for the rotation strip
#define BOARD_HAS_IMU              1    // present + initialized for I2C bus health
#define BOARD_HAS_BATTERY          1
#define BOARD_HAS_IO_EXPANDER      0    // TCA9554 exists on board but only services audio
