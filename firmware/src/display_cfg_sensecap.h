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
// The SenseCAP Indicator PCB routes ESP32-S3 GPIO_n to LCD data bus pin D[(n+5) mod 16].
// Standard RGB565 maps D[4:0]=B, D[10:5]=G, D[15:11]=R.
// To compensate: Arduino_GFX R parameter must use GPIOs 6-10 (→ D[11-15] = LCD R),
//                G parameter must use GPIOs 0-5  (→ D[5-10]  = LCD G),
//                B parameter must use GPIOs 11-15 (→ D[0-4]   = LCD B).
// Empirically confirmed: 0xF800→blue, 0x001F→green with any other assignment.
#define SENSECAP_LCD_R0      6
#define SENSECAP_LCD_R1      7
#define SENSECAP_LCD_R2      8
#define SENSECAP_LCD_R3      9
#define SENSECAP_LCD_R4     10
#define SENSECAP_LCD_G0      0
#define SENSECAP_LCD_G1      1
#define SENSECAP_LCD_G2      2
#define SENSECAP_LCD_G3      3
#define SENSECAP_LCD_G4      4
#define SENSECAP_LCD_G5      5
#define SENSECAP_LCD_B0     11
#define SENSECAP_LCD_B1     12
#define SENSECAP_LCD_B2     13
#define SENSECAP_LCD_B3     14
#define SENSECAP_LCD_B4     15
#define SENSECAP_LCD_SPI_SCK  41
#define SENSECAP_LCD_SPI_MOSI 48
#define SENSECAP_BACKLIGHT    45

// ---- I2C bus (touch + PCA9535 expander) ----
// Confirmed from Seeed ESP-IDF reference: SCL=40, SDA=39.
#define SENSECAP_IIC_SDA  39
#define SENSECAP_IIC_SCL  40

// FT6x36 touch I2C address on this board.
// Note: standard FT6336 address is 0x38, but the D1L board has it at 0x48.
// Confirmed by I2C scan + FT5x06 register probe (CHIP_ID=0x64, DEVICE_MODE=0x00).
#define SENSECAP_TOUCH_ADDR  0x48

// ---- PCA9535 I2C expander ----
// Controls touch RST and other GPIOs not wired directly to ESP32-S3.
// Address confirmed from Seeed schematic (A0=A1=A2=GND → 0x20).
#define SENSECAP_PCA9535_ADDR  0x20
// P07 = touch RST output (EXPANDER_IO_TP_RESET = 7 per Seeed ESP-IDF source).
#define SENSECAP_TP_RST_PIN    7

// ---- User button ----
// Single physical button on the front panel. Verify GPIO against schematic.
#define SENSECAP_BTN  38

// ---- ST7701S init sequence: type9 + COLMOD=0x50 (RGB565) ----
// type9 from Arduino_GFX uses COLMOD=0x60 (RGB666), which misaligns the
// R/G/B channels on a 16-bit parallel bus and produces a green tint.
// This array is byte-for-byte identical to st7701_type9_init_operations
// except the single COLMOD byte is changed from 0x60 to 0x50.
static const uint8_t st7701_sensecap_init_operations[] = {
    BEGIN_WRITE,
    WRITE_COMMAND_8, 0xFF,
    WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x10,

    WRITE_C8_D16, 0xC0, 0x3B, 0x00,
    WRITE_C8_D16, 0xC1, 0x0D, 0x02,
    WRITE_C8_D16, 0xC2, 0x31, 0x05,
    WRITE_C8_D8, 0xCD, 0x00,

    WRITE_COMMAND_8, 0xB0, // Positive Voltage Gamma Control
    WRITE_BYTES, 16,
    0x00, 0x11, 0x18, 0x0E,
    0x11, 0x06, 0x07, 0x08,
    0x07, 0x22, 0x04, 0x12,
    0x0F, 0xAA, 0x31, 0x18,

    WRITE_COMMAND_8, 0xB1, // Negative Voltage Gamma Control
    WRITE_BYTES, 16,
    0x00, 0x11, 0x19, 0x0E,
    0x12, 0x07, 0x08, 0x08,
    0x08, 0x22, 0x04, 0x11,
    0x11, 0xA9, 0x32, 0x18,

    WRITE_COMMAND_8, 0xFF,
    WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x11,

    WRITE_C8_D8, 0xB0, 0x60,
    WRITE_C8_D8, 0xB1, 0x32,
    WRITE_C8_D8, 0xB2, 0x07,
    WRITE_C8_D8, 0xB3, 0x80,
    WRITE_C8_D8, 0xB5, 0x49,
    WRITE_C8_D8, 0xB7, 0x85,
    WRITE_C8_D8, 0xB8, 0x21,
    WRITE_C8_D8, 0xC1, 0x78,
    WRITE_C8_D8, 0xC2, 0x78,

    WRITE_COMMAND_8, 0xE0,
    WRITE_BYTES, 3, 0x00, 0x1B, 0x02,

    WRITE_COMMAND_8, 0xE1,
    WRITE_BYTES, 11,
    0x08, 0xA0, 0x00, 0x00,
    0x07, 0xA0, 0x00, 0x00,
    0x00, 0x44, 0x44,

    WRITE_COMMAND_8, 0xE2,
    WRITE_BYTES, 12,
    0x11, 0x11, 0x44, 0x44,
    0xED, 0xA0, 0x00, 0x00,
    0xEC, 0xA0, 0x00, 0x00,

    WRITE_COMMAND_8, 0xE3,
    WRITE_BYTES, 4, 0x00, 0x00, 0x11, 0x11,

    WRITE_C8_D16, 0xE4, 0x44, 0x44,

    WRITE_COMMAND_8, 0xE5,
    WRITE_BYTES, 16,
    0x0A, 0xE9, 0xD8, 0xA0,
    0x0C, 0xEB, 0xD8, 0xA0,
    0x0E, 0xED, 0xD8, 0xA0,
    0x10, 0xEF, 0xD8, 0xA0,

    WRITE_COMMAND_8, 0xE6,
    WRITE_BYTES, 4, 0x00, 0x00, 0x11, 0x11,

    WRITE_C8_D16, 0xE7, 0x44, 0x44,

    WRITE_COMMAND_8, 0xE8,
    WRITE_BYTES, 16,
    0x09, 0xE8, 0xD8, 0xA0,
    0x0B, 0xEA, 0xD8, 0xA0,
    0x0D, 0xEC, 0xD8, 0xA0,
    0x0F, 0xEE, 0xD8, 0xA0,

    WRITE_COMMAND_8, 0xEB,
    WRITE_BYTES, 7,
    0x02, 0x00, 0xE4, 0xE4,
    0x88, 0x00, 0x40,

    WRITE_C8_D16, 0xEC, 0x3C, 0x00,

    WRITE_COMMAND_8, 0xED,
    WRITE_BYTES, 16,
    0xAB, 0x89, 0x76, 0x54,
    0x02, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0x20,
    0x45, 0x67, 0x98, 0xBA,

    WRITE_COMMAND_8, 0xFF,
    WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x13,

    WRITE_C8_D8, 0xE5, 0xE4,

    WRITE_COMMAND_8, 0xFF,
    WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x00,

    WRITE_C8_D8, 0x3A, 0x50, // RGB565 (type9 uses 0x60 RGB666 which green-shifts on 16-bit bus)

    WRITE_COMMAND_8, 0x11, // Sleep Out
    END_WRITE,

    DELAY, 120,

    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x21, // Display Inversion ON (IPS panel — must follow Sleep Out)
    WRITE_COMMAND_8, 0x29, // Display On
    END_WRITE,
};

// ---- Global hardware objects (defined in main.cpp) ----
extern Arduino_GFX *gfx;
extern TouchLib touch_sc;
extern PCA9535  pca;
