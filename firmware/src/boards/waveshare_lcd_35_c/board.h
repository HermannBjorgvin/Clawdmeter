#pragma once

// Waveshare ESP32-S3-Touch-LCD-3.5 / 3.5-C — 320x480 IPS LCD (ST7796 over
// SPI), FT6336 touch, AXP2101 PMU, QMI8658 IMU, ES8311 codec, TCA9554 IO
// expander. Pins are from Waveshare's ESP-IDF factory example. The display
// reset is TCA9554 EXIO1; the FT6336 has no reset or interrupt wire.

#define BOARD_NAME           "Waveshare LCD 3.5-C"

#define LCD_WIDTH            320
#define LCD_HEIGHT           480

// ---- Display pins (ST7796, 4-wire SPI) ----
#define LCD_DC               3
#define LCD_CS              -1
#define LCD_SCK              5
#define LCD_MOSI             1
#define LCD_RST              -1
#define LCD_BL               6

// ---- I2C bus (touch + PMU + IMU + expander) ----
#define IIC_SDA              8
#define IIC_SCL              7

// ---- Touch (FT6336, FocalTech-style data layout @ regs 0x02..0x06) ----
#define FT6336_ADDR          0x38

// ---- PMU ----
#define AXP2101_ADDR         0x34

// ---- IO expander ----
#define IO_EXPANDER_ADDR     0x20
#define IOX_LCD_RESET        1     // ST7796 reset (active LOW)
#define IOX_PA_CTRL          7     // NS4150B CTRL (HIGH = amp on)

// ---- Buttons ----
#define BTN_BACK_GPIO        0     // BOOT — primary, Space (PTT)

// ---- Audio (ES8311 codec + speaker, I2S) ----
#define SND_I2S_MCLK         12
#define SND_I2S_BCLK         13
#define SND_I2S_WS           15
#define SND_I2S_DOUT         16
#define SND_I2S_DIN          14
#define SND_SAMPLE_RATE      44100
#define SND_ES8311_ADDR      0x18
#define SND_VOLUME           70    // ES8311 0..100

#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         1
#define BOARD_HAS_IMU              1
#define BOARD_HAS_BATTERY          1
#define BOARD_HAS_IO_EXPANDER      1
#define BOARD_HAS_SOUND            1
