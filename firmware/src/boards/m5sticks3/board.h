#pragma once

// M5Stack M5StickS3 — ESP32-S3-PICO-1-N8R8 (8 MB quad flash + 8 MB octal
// PSRAM), 1.14" 135x240 ST7789P3 over plain 4-wire SPI, no touch. Power,
// battery telemetry, the LCD rail and the speaker amp all hang off M5Stack's
// M5PM1 PMIC at I2C 0x6E — see pm1.h. Pin map cross-checked against M5GFX /
// M5Unified (board_M5StickS3 autodetect + bring-up) and the official docs:
// https://docs.m5stack.com/en/core/StickS3

#define BOARD_NAME           "M5StickS3"

// ---- Display geometry ----
#define LCD_WIDTH            135
#define LCD_HEIGHT           240

// ---- SPI display pins (ST7789P3, 4-wire SPI, no MISO) ----
#define LCD_CS               41
#define LCD_SCLK             40
#define LCD_MOSI             39
#define LCD_DC               45
#define LCD_RST              21
#define LCD_BL               38    // backlight, LEDC PWM (active high). The
                                   // panel rail itself is PM1-gated — see
                                   // board_init.cpp.

// ---- Internal I2C bus ----
// M5PM1 PMIC @0x6E, BMI270 IMU @0x68, ES8311 codec @0x18.
#define IIC_SDA              47
#define IIC_SCL              48

// ---- Buttons (active LOW, board pull-ups) ----
// The PMIC's own PEK button handles hard power in hardware (click = on,
// double-click = off) and is not an ESP32 GPIO.
#define BTN_A_GPIO           11    // front — PRIMARY, HID Space (PTT)
#define BTN_B_GPIO           12    // side — PWR-role: toggle screens / hold to pair

// ---- Audio (ES8311 mono codec + AW8737 amp, I2S) ----
// The amp enable is PM1 GPIO3 over I2C, not an ESP32 GPIO — see sound.cpp.
#define SND_I2S_MCLK         18
#define SND_I2S_BCLK         17
#define SND_I2S_WS           15    // LRCK
#define SND_I2S_DOUT         14    // ESP → ES8311 (speaker)
#define SND_I2S_DIN          16    // ES8311 ADC → ESP (mic, unused by the chime)
#define SND_SAMPLE_RATE      44100 // must match the embedded PCM (bell_pcm.h)
#define SND_ES8311_ADDR      0x18

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 0   // BtnB is the PWR-role button (power.cpp)
#define BOARD_HAS_ROTATION         0
#define BOARD_HAS_IMU              0   // BMI270 populated but unused (v1)
#define BOARD_HAS_BATTERY          1
#define BOARD_HAS_IO_EXPANDER      0
#define BOARD_HAS_SOUND            1
