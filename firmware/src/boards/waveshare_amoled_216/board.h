#pragma once

// Waveshare ESP32-S3-Touch-AMOLED-2.16 — original square AMOLED kit.
// 480x480 CO5300 + CST9220 touch + AXP2101 PMU + QMI8658 IMU.
// IMU-driven CPU rotation is enabled.

#define BOARD_NAME           "Waveshare AMOLED 2.16"

// ---- Display geometry (matches BoardCaps; duplicated here as compile-time
// constants because the buffer-size math runs at file scope) ----
#define LCD_WIDTH            480
#define LCD_HEIGHT           480

// ---- QSPI display pins (CO5300) ----
#define LCD_CS               12
#define LCD_SCLK             38
#define LCD_SDIO0            4
#define LCD_SDIO1            5
#define LCD_SDIO2            6
#define LCD_SDIO3            7
#define LCD_RESET            2

// ---- I2C bus (touch + PMU + IMU) ----
#define IIC_SDA              15
#define IIC_SCL              14

// ---- Touch (CST9220 via TouchDrvCST92xx library) ----
#define TP_INT               11
#define TP_RST               2     // shared with LCD_RESET
#define CST9220_ADDR         0x5A

// ---- PMU ----
#define AXP2101_ADDR         0x34

// ---- Buttons ----
#define BTN_BACK_GPIO        0     // BOOT — primary, Space (PTT)
#define BTN_FWD_GPIO         18    // secondary, Shift+Tab (mode toggle)

// ---- Capability flags (compile-time; redundant with BoardCaps but lets
// the linker dead-strip whole functions on boards that don't need them) ----
#define BOARD_HAS_SECONDARY_BUTTON 1
#define BOARD_HAS_ROTATION         1
#define BOARD_HAS_IMU              1
#define BOARD_HAS_BATTERY          1
#define BOARD_HAS_IO_EXPANDER      0
#define BOARD_HAS_AUDIO            0
