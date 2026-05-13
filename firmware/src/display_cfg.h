#pragma once

#include <Arduino_GFX_Library.h>
#include <Arduino_DriveBus_Library.h>
#include <XPowersLib.h>
#include <SensorQMI8658.hpp>
#include <Adafruit_XCA9554.h>
#include <Wire.h>

// ---- Display resolution (Waveshare ESP32-S3-Touch-AMOLED-1.8) ----
#define LCD_WIDTH   368
#define LCD_HEIGHT  448

// ---- QSPI display pins (SH8601) ----
#define LCD_CS      12
#define LCD_SCLK    11
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
// LCD reset is driven through the XCA9554 I2C GPIO expander; no direct GPIO.

// ---- Touch pins (FT3168 via I2C) ----
#define IIC_SDA     15
#define IIC_SCL     14
#define TP_INT      21
// Touch reset is also driven through the XCA9554 expander.

// ---- I2C GPIO expander ----
#define XCA9554_ADDR 0x20

// ---- PMU (AXP2101 via same I2C) ----
#define AXP2101_ADDR 0x34

// ---- Global hardware objects (defined in main.cpp) ----
extern Arduino_DataBus *bus;
extern Arduino_SH8601 *gfx;
extern std::unique_ptr<Arduino_IIC> touch_ft;
extern XPowersPMU pmu;
extern SensorQMI8658 imu;
extern Adafruit_XCA9554 io_expander;
