#pragma once

#include <Arduino_GFX_Library.h>
#include <Wire.h>

// ---- Display resolution (both boards) ----
#define LCD_WIDTH   480
#define LCD_HEIGHT  480

// =============================================================================
// Waveshare ESP32-S3-Touch-LCD-4  (ST7701 RGB LCD, GT911 touch)
// =============================================================================
#ifdef BOARD_WAVESHARE_LCD4

// ---- RGB panel pins (ST7701) ----
#define LCD_DE    40
#define LCD_VSYNC 39
#define LCD_HSYNC 38
#define LCD_PCLK  41
#define LCD_R0    46
#define LCD_R1     3
#define LCD_R2     8
#define LCD_R3    18
#define LCD_R4    17
#define LCD_G0    14
#define LCD_G1    13
#define LCD_G2    12
#define LCD_G3    11
#define LCD_G4    10
#define LCD_G5     9
#define LCD_B0     5
#define LCD_B1    45
#define LCD_B2    48
#define LCD_B3    47
#define LCD_B4    21

// ---- Software SPI for ST7701 init commands ----
#define LCD_SPI_CS    42
#define LCD_SPI_SCK    2
#define LCD_SPI_MOSI   1

// ---- I2C bus (touch GT911) ----
#define IIC_SDA  15
#define IIC_SCL   7

// ---- I2C peripheral expander (0x24) — controls display power/backlight ----
#define IO_EXPANDER_ADDR  0x24

#include <TouchDrvGT911.hpp>

extern Arduino_ESP32RGBPanel *rgbpanel;
extern Arduino_RGB_Display   *gfx;
extern TouchDrvGT911          touch;

// =============================================================================
// Waveshare ESP32-S3-Touch-AMOLED-2.16  (CO5300 QSPI AMOLED, CST9220, AXP2101, QMI8658)
// =============================================================================
#else

#include <TouchDrvCSTXXX.hpp>
#include <XPowersLib.h>
#include <SensorQMI8658.hpp>

// ---- QSPI display pins (CO5300) ----
#define LCD_CS      12
#define LCD_SCLK    38
#define LCD_SDIO0    4
#define LCD_SDIO1    5
#define LCD_SDIO2    6
#define LCD_SDIO3    7
#define LCD_RESET    2

// ---- Touch pins (CST9220 via I2C) ----
#define IIC_SDA      15
#define IIC_SCL      14
#define TP_INT       11
#define TP_RST        2    // shared with LCD_RESET
#define CST9220_ADDR 0x5A

// ---- PMU (AXP2101 via same I2C) ----
#define AXP2101_ADDR 0x34

extern Arduino_DataBus *bus;
extern Arduino_CO5300  *gfx;
extern TouchDrvCST92xx  touch;
extern XPowersPMU       pmu;
extern SensorQMI8658    imu;

#endif // BOARD_WAVESHARE_LCD4
