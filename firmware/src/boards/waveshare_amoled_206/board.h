#pragma once

// Waveshare ESP32-S3-Touch-AMOLED-2.06 — watch-form-factor AMOLED kit.
// 410x502 CO5300 + FT3168 touch + AXP2101 PMU + QMI8658 IMU + PCF85063 RTC
// + ES8311 codec. No IO expander; LCD and touch resets are direct GPIOs.
// IMU is present but rotation is disabled (watch enclosure is fixed-orientation).

#define BOARD_NAME           "Waveshare AMOLED 2.06"

// ---- Display geometry ----
#define LCD_WIDTH            410
#define LCD_HEIGHT           502

// ---- QSPI display pins (CO5300) — from Waveshare's pin_config.h ----
#define LCD_CS               12
#define LCD_SCLK             11        // differs from 2.16 (GPIO 38)
#define LCD_SDIO0            4
#define LCD_SDIO1            5
#define LCD_SDIO2            6
#define LCD_SDIO3            7
#define LCD_RESET            8         // direct GPIO, not via expander

// ---- I2C bus (PMU + IMU + RTC + codec share this bus) ----
#define IIC_SDA              15
#define IIC_SCL              14

// ---- Touch (FT3168 via vendored minimal I2C reader; shares I2C with PMU) ----
#define TP_INT               38
#define TP_RESET             9
#define FT3168_ADDR          0x38

// ---- PMU ----
#define AXP2101_ADDR         0x34

// ---- Buttons ----
#define BTN_BACK_GPIO        0     // BOOT — primary, Space (PTT)
// PWR button uses AXP2101 PKEY short-press IRQ (no IO expander on this board,
// despite what the wiki FAQ implies — see schematic, Key3 wires to AXP PWRON).
// There is no third button on this board.

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         0   // watch form factor: fixed orientation
#define BOARD_HAS_IMU              1   // present + initialized, but rotation off
#define BOARD_HAS_BATTERY          1
#define BOARD_HAS_IO_EXPANDER      0
