#pragma once

// LilyGO T4-S3 — 450x600 portrait AMOLED kit.
// RM690B0 + CST226SE touch + SY6970 charger. No IMU, no PMU push-button,
// no second GPIO button (GPIO 18 is the panel TE line). Single physical
// input is the BOOT button; a long hold synthesizes a PWR press so the
// shared screen-cycling path still works.

#define BOARD_NAME           "LilyGO T4-S3"

// ---- Display geometry ----
#define LCD_WIDTH            450
#define LCD_HEIGHT           600

// The RM690B0 controller addresses a 482-wide RAM but only the middle
// 450 columns are wired to glass — every draw needs col_offset = 16 so
// pixels land inside the visible viewport.
#define LCD_COL_OFFSET       16

// ---- QSPI display pins (RM690B0) ----
#define LCD_CS               11
#define LCD_SCLK             15
#define LCD_SDIO0            14
#define LCD_SDIO1            10
#define LCD_SDIO2            16
#define LCD_SDIO3            12
#define LCD_RESET            13
// GPIO 18 is the panel's Tearing-Effect line — reserved, do not reuse.

// ---- I2C bus (touch + PMU share one bus) ----
#define IIC_SDA              6
#define IIC_SCL              7

// ---- Touch (CST226SE via SensorLib TouchClassCST226) ----
#define TP_INT               8
#define TP_RST               17
#define CST226_ADDR          0x5A

// ---- PMU (SY6970 charger; not a fuel gauge) ----
#define SY6970_ADDR          0x6A
#define PMU_IRQ              5
#define PMICEN_GPIO          9     // drive HIGH to power the PMU's LDO rails

// ---- Buttons ----
#define BTN_BACK_GPIO        0     // BOOT — primary, Space (PTT)
// No second GPIO button on this board. The SY6970 has no user push-button
// either, so screen cycling is mapped to a long press of BOOT — see
// power.cpp. Keep this threshold above a comfortable voice-mode PTT hold.
#define PWR_LONGPRESS_MS     1500

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         0
#define BOARD_HAS_IMU              0
#define BOARD_HAS_BATTERY          1
#define BOARD_HAS_IO_EXPANDER      0
