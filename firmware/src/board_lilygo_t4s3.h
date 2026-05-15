#pragma once

#include <Arduino_GFX_Library.h>
#include <TouchDrvCSTXXX.hpp>
#include <XPowersLib.h>
#include <Wire.h>

// ---- Display: RM690B0 450x600 portrait AMOLED via QSPI ----
#define LCD_WIDTH       450
#define LCD_HEIGHT      600
#define LCD_COL_OFFSET  16   // 482px-wide controller, 450px visible → center the 450 by skipping 16
#define LCD_CS      11
#define LCD_SCLK    15
#define LCD_SDIO0   14
#define LCD_SDIO1   10
#define LCD_SDIO2   16
#define LCD_SDIO3   12
#define LCD_RESET   13
#define LCD_TE      18         // reserved by panel; do not reuse as GPIO

// ---- Touch: CST226SE via I2C ----
#define IIC_SDA     6
#define IIC_SCL     7
#define TP_INT      8
#define TP_RST      17
#define TOUCH_ADDR  0x5A

// ---- PMU: SY6970 charger (shares I2C with touch) ----
#define PMU_ADDR    0x6A       // SY6970_SLAVE_ADDRESS per XPowersLib
#define PMU_IRQ     5
#define PMU_EN      9          // PMICEN: drive HIGH to enable PMU rail

// ---- Capability flags ----
#define HAS_IMU            0   // no IMU on this board
#define HAS_PMU_BUTTON     0   // SY6970 has no user button
#define HAS_BTN_FWD        0   // GPIO 18 is reserved for display TE

// ---- Driver type aliases ----
typedef Arduino_RM690B0  DisplayDriver;
typedef TouchClassCST226 TouchDriver;
typedef PowersSY6970     PmuDriver;
// (no ImuDriver — guarded by HAS_IMU)

// ---- Global hardware objects (defined in main.cpp) ----
extern Arduino_DataBus *bus;
extern DisplayDriver   *gfx;
extern TouchDriver      touch;
extern PmuDriver        pmu;
