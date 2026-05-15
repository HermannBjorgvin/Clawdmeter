#pragma once

#include <Arduino_GFX_Library.h>
#include <TouchLib.h>
#include <PCA95x5.h>
#include <Wire.h>

// ---- Display resolution (same as Waveshare) ----
#define LCD_WIDTH   480
#define LCD_HEIGHT  480

// ---- RGB parallel display pins (ST7701S) ----
#define SENSECAP_LCD_DE     18
#define SENSECAP_LCD_VSYNC  17
#define SENSECAP_LCD_HSYNC  16
#define SENSECAP_LCD_PCLK   21
#define SENSECAP_LCD_R0      0
#define SENSECAP_LCD_R1      1
#define SENSECAP_LCD_R2      2
#define SENSECAP_LCD_R3      3
#define SENSECAP_LCD_R4      4
#define SENSECAP_LCD_G0      5
#define SENSECAP_LCD_G1      6
#define SENSECAP_LCD_G2      7
#define SENSECAP_LCD_G3      8
#define SENSECAP_LCD_G4      9
#define SENSECAP_LCD_G5     10
#define SENSECAP_LCD_B0     11
#define SENSECAP_LCD_B1     12
#define SENSECAP_LCD_B2     13
#define SENSECAP_LCD_B3     14
#define SENSECAP_LCD_B4     15
#define SENSECAP_LCD_SPI_SCK  41
#define SENSECAP_LCD_SPI_MOSI 48
#define SENSECAP_BACKLIGHT    45

// ---- I2C bus (touch + PCA9535 expander) ----
#define SENSECAP_IIC_SDA  40
#define SENSECAP_IIC_SCL  39

// ---- PCA9535 I2C expander ----
// Controls touch RST and other GPIOs not wired directly to ESP32-S3.
// Address confirmed from Seeed schematic (A0=A1=A2=GND → 0x20).
#define SENSECAP_PCA9535_ADDR  0x20
// P06 = touch RST output. Verify pin number against the SenseCAP Indicator
// schematic before flashing if touch does not initialise.
#define SENSECAP_TP_RST_PIN    6

// ---- User button ----
// Single physical button on the front panel. Verify GPIO against schematic.
#define SENSECAP_BTN  38

// ---- Global hardware objects (defined in main.cpp) ----
extern Arduino_GFX *gfx;
extern TouchLib touch_sc;
extern PCA9535  pca;
