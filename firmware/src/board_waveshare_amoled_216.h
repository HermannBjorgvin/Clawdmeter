#pragma once

#include <Arduino_GFX_Library.h>
#include <TouchDrvCSTXXX.hpp>
#include <XPowersLib.h>
#include <SensorQMI8658.hpp>
#include <Wire.h>

// ---- Display: CO5300 480x480 square AMOLED via QSPI ----
#define LCD_WIDTH   480
#define LCD_HEIGHT  480
#define LCD_CS      12
#define LCD_SCLK    38
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_RESET   2

// ---- Touch: CST9220 via I2C ----
#define IIC_SDA     15
#define IIC_SCL     14
#define TP_INT      11
#define TP_RST      2          // shared with LCD_RESET
#define TOUCH_ADDR  0x5A

// ---- PMU: AXP2101 on same I2C bus ----
#define PMU_ADDR    0x34

// ---- Capability flags ----
#define HAS_IMU            1   // QMI8658 accelerometer for auto-rotation
#define HAS_PMU_BUTTON     1   // AXP2101 PKEY → middle button
#define HAS_BTN_FWD        1   // GPIO 18 → Shift+Tab
#define BTN_FWD_GPIO       18

// ---- Driver type aliases (chosen per board) ----
typedef Arduino_CO5300   DisplayDriver;
typedef TouchDrvCST92xx  TouchDriver;
typedef XPowersPMU       PmuDriver;
typedef SensorQMI8658    ImuDriver;

// ---- Global hardware objects (defined in main.cpp) ----
extern Arduino_DataBus *bus;
extern DisplayDriver   *gfx;
extern TouchDriver      touch;
extern PmuDriver        pmu;
extern ImuDriver        imu;
