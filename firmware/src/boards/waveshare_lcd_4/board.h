#pragma once

// Waveshare ESP32-S3-Touch-LCD-4 — 4" square RGB TFT kit.
// 480x480 ST7701 (RGB parallel + SW SPI init) + GT911 touch + an IO expander
// for display power / backlight: CH32V003 at 0x24 (V4.0) or TCA9554 at 0x20
// (earlier revisions). No AXP2101, no IMU.
// Module is ESP32-S3R8 (8 MB OPI PSRAM, 16 MB flash).
//
// Pin map matches the hardware-tested lcd4 fork and Waveshare's wiki
// (RGB data pins are 0-based R0..R4 / G0..G5 / B0..B4 here; the wiki labels
// them R1..R5 / B1..B5). Later board revs share a single I2C bus (SDA=15,
// SCL=7) for both the expander and GT911 — GPIO 8/9 are RGB data and cannot
// be used as a second I2C once the panel is running.

#define BOARD_NAME           "Waveshare LCD 4"

// ---- Display geometry ----
#define LCD_WIDTH            480
#define LCD_HEIGHT           480
// Fixed UI orientation, in quarter turns clockwise of the image on the panel:
//   0 = native, 1 = 90° CW (USB port at the bottom), 2 = 180°, 3 = 270° CW.
// Applied in display.cpp (per-strip pixel remap) and touch.cpp (inverse map);
// the panel is square so LVGL's W×H is unchanged.
#define LCD_ROTATION         0

// ---- RGB panel pins (ST7701) ----
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

// ---- Software SPI for ST7701 init commands ----
#define LCD_SPI_CS           42
#define LCD_SPI_SCK           2
#define LCD_SPI_MOSI          1

// ---- I2C bus (GT911 + IO expander) ----
#define IIC_SDA              15
#define IIC_SCL               7

// ---- Touch (GT911, polled — INT is GPIO 16 on the wiki but unused here) ----
#define TP_INT               16

// ---- IO expander ----
// Two board revisions, same job (display power/reset, touch reset, buzzer).
// Must be programmed before gfx->begin() or the panel stays dark.
//
// CH32V003 microcontroller-as-expander @ 0x24 (current boards). Register and
// pin map from Waveshare's own WS_CH32_IO library for this board:
#define IO_EXPANDER_ADDR     0x24
#define CH32_REG_DIRECTION   0x02  // 1 = output
#define CH32_REG_OUTPUT      0x03
#define CH32_REG_PWM         0x05  // backlight PWM 0..255, inverted: higher = dimmer
#define CH32_REG_ADC         0x06  // battery ADC, 2 bytes little-endian, 10-bit
#define CH32_ADC_REF_V       3.3f
#define CH32_BAT_DIVIDER     3.0f  // VBAT = ADC volts × 3 (Waveshare WS_CH32_IO)
#define CH32_REG_INPUT       0x04
#define CH32_PIN_CHG_STAT    0     // input: ETA6098 charger STAT (low = charging), V4.0 schematic
#define CH32_PIN_TOUCH_RST   1
#define CH32_PIN_LCD_RST     3
#define CH32_PIN_SYS_EN      5
#define CH32_PIN_BUZZER      6     // BEE_EN — keep LOW
#define CH32_PIN_RTC_INT     7
//
// TCA9554 @ 0x20 (V1–V3 boards). Not tested on hardware since the port first
// shipped, so io_expander.cpp writes exactly what it always has: output 0xFF
// (to 0x01 and 0x02), then config 0x3A, and the backlight as latch bit 2.
// The wiki's table (P0 TP_RST, P1 BL_EN, P2 LCD_RST, P3 SD_CS, P5 BEE_EN,
// P6 RTC_INT) suggests that leaves BL_EN and BEE_EN as inputs, weakly pulled
// up by the TCA9554's internal resistors, and that bit 2 is LCD_RST, not the
// backlight. Change it only with a TCA board to test on.
#define IO_EXPANDER_ADDR_ALT 0x20
#define TCA_REG_OUTPUT       0x01
#define TCA_REG_POLARITY     0x02
#define TCA_REG_CONFIG       0x03  // 1 = input
#define TCA_OUTPUT_ON        0xFF
#define TCA_CONFIG           0x3A
#define TCA_PIN_BACKLIGHT    2

// ---- Buttons ----
#define BTN_BACK_GPIO        0     // BOOT — primary, Space (PTT)
// KEY/PWR is wired to EN/RST (hardware reset), not a GPIO. No secondary
// button — GPIO 18 is display R3.

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         0
#define BOARD_HAS_IMU              0
#define BOARD_HAS_BATTERY          1   // optional 3.7 V LiPo on the PH2.0 jack
#define BOARD_HAS_IO_EXPANDER      1
#define BOARD_HAS_SOUND            0
