#pragma once

// Waveshare ESP32-S3-Touch-AMOLED-1.32 — 466x466 round AMOLED.
// ESP32-S3-PICO-1-N8R8 (8 MB quad flash + 8 MB octal PSRAM, esptool-verified).
// CO5300 + CST820 touch + ES8311 codec. No PMU, no IMU, no IO expander.
// Pins from Waveshare's 04_BATT_PWR_Test example (user_config.h, button_bsp.c,
// vbat_bsp.cpp) for this exact board.

#define BOARD_NAME           "Waveshare AMOLED 1.32"

#define LCD_WIDTH            466
#define LCD_HEIGHT           466

// ---- QSPI display pins (CO5300) ----
#define LCD_CS               10
#define LCD_SCLK             11
#define LCD_SDIO0            12
#define LCD_SDIO1            13
#define LCD_SDIO2            14
#define LCD_SDIO3            15
#define LCD_RESET            8
// The 466-wide viewport starts at GRAM column 6 (Waveshare's flush adds 0x06).
#define CO5300_COL_OFFSET    6

// ---- I2C bus (touch + ES8311) ----
#define IIC_SDA              47
#define IIC_SCL              48

// ---- Touch (CST820, FocalTech-style regs 0x02..0x06) ----
#define CST820_ADDR          0x15
#define TP_RST               7
#define TP_INT               6

// ---- Power / battery (no PMU) ----
// SYS_POWER (GPIO18) is the battery power-hold latch: HIGH keeps the rail up.
// VBAT is a 2:1 divider into GPIO4 (ADC1_CH3). No charge-status pin.
#define BAT_EN               18
#define BAT_ADC_PIN          4
#define BAT_VOLT_DIVIDER     2.0f

// ---- Buttons ----
// BOOT is repurposed as "rotate the UI 90° CW" (see imu.cpp) instead of HID
// Space. PWR keeps the shared role: short press cycles brightness,
// hold ~3 s + release pairs, hold 8 s powers off.
#define BTN_ROTATE_GPIO      0     // BOOT, active LOW
#define BTN_PWR_GPIO         17    // PWR, active LOW

#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         1   // button-driven, not IMU-driven
#define BOARD_HAS_IMU              0
#define BOARD_HAS_BATTERY          1
#define BOARD_HAS_IO_EXPANDER      0
#define BOARD_HAS_SOUND            0
