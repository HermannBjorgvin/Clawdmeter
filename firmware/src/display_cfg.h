#pragma once

#include <Arduino_GFX_Library.h>
#include <TouchDrvCSTXXX.hpp>
#include <XPowersLib.h>
#include <SensorQMI8658.hpp>
#include <Wire.h>

// ---- Display resolution ----
#define LCD_WIDTH   480
#define LCD_HEIGHT  480

// ---- QSPI display pins (CO5300) ----
#define LCD_CS      12
#define LCD_SCLK    38
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_RESET   2

// ---- Touch pins (CST9220 via I2C) ----
#define IIC_SDA     15
#define IIC_SCL     14
#define TP_INT      11
#define TP_RST      2    // shared with LCD_RESET
#define CST9220_ADDR 0x5A

// ---- PMU (AXP2101 via same I2C) ----
#define AXP2101_ADDR 0x34

// ---- Global hardware objects (defined in main.cpp) ----
extern Arduino_DataBus *bus;
extern Arduino_CO5300 *gfx;
extern TouchDrvCST92xx touch;
extern XPowersPMU pmu;
extern SensorQMI8658 imu;
