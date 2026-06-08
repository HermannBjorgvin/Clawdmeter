#pragma once

#define BOARD_NAME  "ESP32-S3 4848S040"

// ---- Display geometry ----
#define LCD_WIDTH   480
#define LCD_HEIGHT  480

// ---- ST7701S: 3-wire SPI for init commands ----
#define TFT_CS      39
#define TFT_SCLK    48
#define TFT_MOSI    47

// ---- ST7701S: RGB parallel pixel interface ----
#define TFT_DE      18
#define TFT_VSYNC   17
#define TFT_HSYNC   16
#define TFT_PCLK    21
// Red channel (bit 0 = LSB)
#define TFT_R0      11
#define TFT_R1      12
#define TFT_R2      13
#define TFT_R3      14
#define TFT_R4      0
// Green channel
#define TFT_G0      8
#define TFT_G1      20
#define TFT_G2      3
#define TFT_G3      46
#define TFT_G4      9
#define TFT_G5      10
// Blue channel
#define TFT_B0      4
#define TFT_B1      5
#define TFT_B2      6
#define TFT_B3      7
#define TFT_B4      15

// ---- Sync timing (from Generic Guition datasheet / OpenHASP reference) ----
#define TFT_HSYNC_POLARITY      1
#define TFT_HSYNC_FRONT_PORCH   10
#define TFT_HSYNC_PULSE_WIDTH   8
#define TFT_HSYNC_BACK_PORCH    50
#define TFT_VSYNC_POLARITY      1
#define TFT_VSYNC_FRONT_PORCH   10
#define TFT_VSYNC_PULSE_WIDTH   8
#define TFT_VSYNC_BACK_PORCH    20
#define TFT_PCLK_ACTIVE_NEG     1
#define TFT_PREFER_SPEED        12000000L

// ---- Backlight (PWM, active-high) ----
#define TFT_BCKL    38

// ---- I2C bus (touch only on this board) ----
#define IIC_SDA     19
#define IIC_SCL     45

// ---- Touch: GT911 ----
// No IRQ or RST pins connected; probe both possible addresses at init.
#define GT911_ADDR_PRIMARY  0x5D
#define GT911_ADDR_ALT      0x14

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON  0
#define BOARD_HAS_ROTATION          0
#define BOARD_HAS_IMU               0
#define BOARD_HAS_BATTERY           0
#define BOARD_HAS_IO_EXPANDER       0
