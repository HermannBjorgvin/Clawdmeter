#pragma once

// Waveshare ESP32-S3-Touch-LCD-2.1 — 2.1" round RGB TFT kit.
// 480x480 ST7701S (RGB parallel + 9-bit SW SPI init) + CST820 touch +
// TCA9554 IO expander at 0x20 (LCD/touch resets, LCD SPI CS, buzzer).
// QMI8658 IMU and PCF85063 RTC are populated but unused here.
// Module is ESP32-S3R8 (8 MB OPI PSRAM; IO33-37 "internal occupancy" on the
// schematic) with a 16 MB W25Q128 flash.
//
// Pins are from Waveshare's schematic and ESP-IDF demo. The schematic labels
// the RGB lines R1..R5 / B1..B5 (R0/B0 are NC); here they are 0-based.
//
// BOARD_LCD28C (env waveshare_lcd_28c, extends this env) builds the
// Waveshare ESP32-S3-Touch-LCD-2.8C from this folder: same wiring, but a GT911
// touch controller, its own ST7701S init table and vsync porches, and its
// demo's battery calibration. The plain 2.8 (no touch) isn't supported.

#ifdef BOARD_LCD28C
#define BOARD_NAME           "Waveshare LCD 2.8C"
#define BOARD_LOG_TAG        "LCD-2.8C"
#else
#define BOARD_NAME           "Waveshare LCD 2.1"
#define BOARD_LOG_TAG        "LCD-2.1"
#endif

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

// ---- RGB vsync porches (each board's Waveshare demo; hsync 50/8/10 on both) ----
#ifdef BOARD_LCD28C
#define LCD_VSYNC_PULSE       2
#define LCD_VSYNC_BACK       18
#else
#define LCD_VSYNC_PULSE       3
#define LCD_VSYNC_BACK        8
#endif

// ---- Software SPI for ST7701S init commands (CS is expander EXIO3) ----
#define LCD_SPI_SCK           2
#define LCD_SPI_MOSI          1

// ---- Backlight: N-MOSFET low side, GPIO high = on, LEDC PWM ----
#define LCD_BL                6

// ---- I2C bus (CST820, TCA9554, QMI8658, PCF85063) ----
#define IIC_SDA              15
#define IIC_SCL               7

// ---- Touch: CST820 (2.1) or GT911 (2.8C) ----
#ifdef BOARD_LCD28C
// board_init() holds INT low through the GT911's reset, which latches 0x5D
// (the 2.8C demo's address); SensorLib also probes 0x14.
#define GT911_ADDR_PRIMARY   0x5D
#define GT911_ADDR_ALT       0x14
#else
#define CST820_ADDR          0x15
#endif
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

// ---- Battery (ETA6098 charger, no I2C; 200k/100k divider on BAT_ADC) ----
#define BAT_ADC_PIN           4
#ifdef BOARD_LCD28C
// The 2.8C demo's BAT_Driver computes mV * 3 / 0.980952 (its
// Measurement_offset calibration), about 2 % up.
#define BAT_VOLT_DIVIDER     (3.0f / 0.980952f)
#else
#define BAT_VOLT_DIVIDER     3.0f
#endif

// ---- Buttons ----
#define BTN_BACK_GPIO        0     // BOOT — primary, Space (PTT)
// RESET is wired to EN, and SW1 is the battery power slide switch: neither
// reaches the firmware, so there is no secondary or PWR-role button.

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         0
#define BOARD_HAS_IMU              0
#define BOARD_HAS_BATTERY          1
#define BOARD_HAS_IO_EXPANDER      1
#define BOARD_HAS_SOUND            0
