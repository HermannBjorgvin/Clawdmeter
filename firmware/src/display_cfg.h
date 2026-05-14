#pragma once

#include <Arduino_GFX_Library.h>
#include <XPowersLib.h>
#include <SensorQMI8658.hpp>
#include <Wire.h>

// ---- Per-board pin / driver configuration ----
#ifdef BOARD_AMOLED_18
    // Waveshare ESP32-S3-Touch-AMOLED-1.8 (368x448 SH8601, FT3168 touch,
    // resets via TCA9554 I2C expander).
    #include <TouchDrvFT6X36.hpp>

    #define LCD_W          368
    #define LCD_H          448

    // QSPI display pins (SH8601)
    #define LCD_CS         12
    #define LCD_SCLK       11
    #define LCD_SDIO0      4
    #define LCD_SDIO1      5
    #define LCD_SDIO2      6
    #define LCD_SDIO3      7
    // Reset is on TCA9554 P0, not an ESP32 GPIO
    #define LCD_RESET      GFX_NOT_DEFINED

    // Touch pins (FT3168 via I2C, shared bus with PMU)
    #define IIC_SDA        15
    #define IIC_SCL        14
    #define TP_INT         21
    #define TP_RST         -1            // on TCA9554 P1
    #define FT3168_ADDR    0x38

    // TCA9554 I/O expander — drives LCD_RST (P0), TP_RST (P1), AUX_RST (P2)
    #define TCA9554_ADDR             0x20
    #define TCA9554_PIN_LCD_RST      0
    #define TCA9554_PIN_TP_RST       1
    #define TCA9554_PIN_AUX_RST      2

    extern TouchDrvFT6X36 touch;

#else
    // Waveshare ESP32-S3-Touch-AMOLED-2.16 (480x480 CO5300, CST9220 touch).
    #include <TouchDrvCSTXXX.hpp>

    #define LCD_W          480
    #define LCD_H          480

    #define LCD_CS         12
    #define LCD_SCLK       38
    #define LCD_SDIO0      4
    #define LCD_SDIO1      5
    #define LCD_SDIO2      6
    #define LCD_SDIO3      7
    #define LCD_RESET      2

    #define IIC_SDA        15
    #define IIC_SCL        14
    #define TP_INT         11
    #define TP_RST         2                // shared with LCD_RESET
    #define CST9220_ADDR   0x5A

    extern TouchDrvCST92xx touch;
#endif

// Legacy aliases — much of the codebase still uses LCD_WIDTH/LCD_HEIGHT.
// Keep both names valid; LCD_W/LCD_H is preferred for new code.
#define LCD_WIDTH      LCD_W
#define LCD_HEIGHT     LCD_H

// ---- PMU (AXP2101 via I2C, same on both boards) ----
#define AXP2101_ADDR   0x34

// ---- Global hardware objects (defined in main.cpp) ----
extern Arduino_DataBus *bus;
#ifdef BOARD_AMOLED_18
    extern Arduino_SH8601 *gfx;
#else
    extern Arduino_CO5300 *gfx;
#endif
extern XPowersPMU pmu;
extern SensorQMI8658 imu;
