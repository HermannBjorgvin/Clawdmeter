#pragma once

// Waveshare ESP32-S3-Touch-LCD-2.8C — 2.8" round RGB TFT kit.
// 480x480 ST7701S (RGB parallel + 9-bit SW SPI init) + GT911 touch +
// TCA9554 IO expander at 0x20 (LCD/touch resets, LCD SPI CS, buzzer).
// QMI8658 IMU and PCF85063 RTC are populated but unused here.
// Module is ESP32-S3R8 (8 MB OPI PSRAM) with 16 MB flash.
//
// Pins are from Waveshare's wiki and ESP-IDF demo; they match the LCD-2.1.
// The wiki labels the RGB lines R1..R5 / B1..B5 (R0/B0 are NC); here they are 0-based.

#define BOARD_NAME           "Waveshare LCD 2.8C"

// ---- Display geometry ----
#define LCD_WIDTH            480
#define LCD_HEIGHT           480

// ---- RGB panel pins (ST7701S) ----
#define LCD_DE               40
#define LCD_VSYNC            39
#define LCD_HSYNC            38
#define LCD_PCLK             41
#define LCD_R0               46
#define LCD_R1                3
#define LCD_R2                8
#define LCD_R3               18
#define LCD_R4               17
#define LCD_G0               14
#define LCD_G1               13
#define LCD_G2               12
#define LCD_G3               11
#define LCD_G4               10
#define LCD_G5                9
#define LCD_B0                5
#define LCD_B1               45
#define LCD_B2               48
#define LCD_B3               47
#define LCD_B4               21

// ---- Software SPI for ST7701S init commands (CS is expander EXIO3) ----
#define LCD_SPI_SCK           2
#define LCD_SPI_MOSI          1

// ---- Backlight: GPIO high = on, LEDC PWM (as on the LCD-2.1) ----
#define LCD_BL                6

// ---- I2C bus (GT911, TCA9554, QMI8658, PCF85063) ----
#define IIC_SDA              15
#define IIC_SCL               7

// ---- Touch (GT911) ----
// board_init() holds INT low through the reset, which latches 0x5D (the
// demo's address); 0x14 is probed as a fallback.
#define GT911_ADDR_PRIMARY   0x5D
#define GT911_ADDR_ALT       0x14
#define TP_INT               16
// TP_RST is expander EXIO2.

// ---- IO expander (TCA9554, A0-A2 tied low) ----
// Pins are the schematic's 1-based EXIO numbers minus one (bit index).
#define IO_EXPANDER_ADDR     0x20
#define IOX_BIT_LCD_RST      0     // EXIO1
#define IOX_BIT_TP_RST       1     // EXIO2
#define IOX_BIT_LCD_CS       2     // EXIO3, active low
#define IOX_BIT_SD_CS        3     // EXIO4
// EXIO5/6 = QMI8658 INT2/INT1, EXIO7 = RTC INT: left as inputs.
#define IOX_BIT_BUZZER       7     // EXIO8, active buzzer, high = on

// ---- Battery (standalone charger, no I2C; x3 divider on BAT_ADC per the demo) ----
#define BAT_ADC_PIN           4
#define BAT_VOLT_DIVIDER     3.0f

// ---- Buttons ----
#define BTN_BACK_GPIO        0     // BOOT — primary, Space (PTT)
// RESET is wired to EN, and the other key is the battery power control:
// neither reaches the firmware, so there is no secondary or PWR-role button.

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         0
#define BOARD_HAS_IMU              0
#define BOARD_HAS_BATTERY          1
#define BOARD_HAS_IO_EXPANDER      1
#define BOARD_HAS_SOUND            0
