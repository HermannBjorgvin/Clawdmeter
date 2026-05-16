#pragma once

#include <Arduino_GFX_Library.h>
#include <Wire.h>

#ifdef BOARD_XINGZHI_CUBE
// ---- xingzhi-cube 1.83" 2mic (ESP32-S3) ----
// Display: ST7789 SPI LCD, 284×240 landscape
// No touch, no PMU, no IMU on this board.

#define LCD_WIDTH    284
#define LCD_HEIGHT   240

// SPI LCD pins
#define LCD_SPI_CLK  9
#define LCD_SPI_MOSI 10
#define LCD_CS       14
#define LCD_DC       8
#define LCD_RST      18
#define LCD_BL       13    // LEDC PWM backlight (TIMER_0, CH_0)

// Buttons (all active LOW with internal pull-ups)
//   BTN_WAKE (GPIO  0) — Space / voice push-to-talk (press + release tracked)
//   BTN_VOL  (GPIO 40) — Shift+Tab / mode toggle
//   BTN_MID  (GPIO 39) — cycle screens / cycle splash animations
#define BTN_WAKE     0
#define BTN_VOL      40
#define BTN_MID      39

// WS2812 RGB LED — reserved, not used by Clawdmeter firmware
#define LED_WS2812   48

// Global display object (base-class pointer; concrete type is Arduino_ST7789)
extern Arduino_GFX *gfx;

#else  // ---- Waveshare ESP32-S3-Touch-AMOLED-2.16 (default) ----
// Display: CO5300 QSPI AMOLED 480×480

#include <TouchDrvCSTXXX.hpp>
#include <XPowersLib.h>
#include <SensorQMI8658.hpp>

#define LCD_WIDTH   480
#define LCD_HEIGHT  480

// QSPI display pins (CO5300)
#define LCD_CS      12
#define LCD_SCLK    38
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_RESET   2

// I2C bus (touch + PMU + IMU share one bus)
#define IIC_SDA     15
#define IIC_SCL     14
#define TP_INT      11
#define TP_RST      2    // shared with LCD_RESET
#define CST9220_ADDR 0x5A
#define AXP2101_ADDR 0x34

// Physical buttons
#define BTN_BACK 0
#define BTN_FWD  18

// Global hardware objects (defined in main.cpp)
extern Arduino_GFX     *gfx;
extern TouchDrvCST92xx  touch;
extern XPowersPMU       pmu;
extern SensorQMI8658    imu;

#endif // BOARD_XINGZHI_CUBE
