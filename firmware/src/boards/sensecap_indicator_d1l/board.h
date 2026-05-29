#pragma once

#define BOARD_NAME  "SenseCap Indicator D1L"

#define LCD_WIDTH   480
#define LCD_HEIGHT  480

// RGB parallel display pins (ST7701S)
// PCB routes ESP32-S3 GPIO_n to LCD data bus D[15-n] (bit-reversal)
#define SENSECAP_LCD_DE     18
#define SENSECAP_LCD_VSYNC  17
#define SENSECAP_LCD_HSYNC  16
#define SENSECAP_LCD_PCLK   21
#define SENSECAP_LCD_R0      4
#define SENSECAP_LCD_R1      3
#define SENSECAP_LCD_R2      2
#define SENSECAP_LCD_R3      1
#define SENSECAP_LCD_R4      0
#define SENSECAP_LCD_G0     10
#define SENSECAP_LCD_G1      9
#define SENSECAP_LCD_G2      8
#define SENSECAP_LCD_G3      7
#define SENSECAP_LCD_G4      6
#define SENSECAP_LCD_G5      5
#define SENSECAP_LCD_B0     15
#define SENSECAP_LCD_B1     14
#define SENSECAP_LCD_B2     13
#define SENSECAP_LCD_B3     12
#define SENSECAP_LCD_B4     11
#define SENSECAP_LCD_SPI_SCK  41
#define SENSECAP_LCD_SPI_MOSI 48
#define SENSECAP_BACKLIGHT    45

// I2C bus (touch + PCA9535 expander)
#define IIC_SDA  39
#define IIC_SCL  40

// Touch (FT6x36, non-standard address on this board)
#define SENSECAP_TOUCH_ADDR  0x48

// PCA9535 I2C expander
// P04 = ST7701S SPI CS (active LOW during init sequence)
// P07 = touch RST (active LOW pulse to reset)
#define SENSECAP_PCA9535_ADDR  0x20
#define SENSECAP_LCD_CS_PORT   4
#define SENSECAP_TP_RST_PIN    7

// Physical button
// GPIO 38 used as edge-detected PWR button (screen cycle / wake from sleep).
// NOT wired as INPUT_BTN_PRIMARY — device has no microphone for PTT.
#define BTN_BACK_GPIO  38

// Capability flags
#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         0
#define BOARD_HAS_IMU              0
#define BOARD_HAS_BATTERY          0
#define BOARD_HAS_IO_EXPANDER      1
