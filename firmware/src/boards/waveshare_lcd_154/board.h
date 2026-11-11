#pragma once

// Waveshare ESP32-S3-Touch-LCD-1.54 — 1.54" square TFT kit.
// 240x240 ST7789 (plain 4-wire SPI, not QSPI) + CST816T touch + battery ADC
// (no PMU — the ETA6098 charger is standalone) + ES8311 codec + speaker.
// QMI8658 IMU is populated but unused (fixed enclosure orientation).
//
// Pin map taken from the BambuHelper project's ws_lcd_154 configuration,
// which is hardware-tested on this exact kit (display, touch, buttons,
// battery ADC and ES8311 all exercised there).

#define BOARD_NAME           "Waveshare LCD 1.54"

// ---- Display geometry ----
#define LCD_WIDTH            240
#define LCD_HEIGHT           240

// ---- Display rotation (ST7789 MADCTL, Arduino_GFX convention) ----
// 0 = factory orientation, 1 = 90° clockwise, 2 = 180°, 3 = 270° (90° CCW).
// The panel is square, so LVGL keeps seeing 240x240; touch.cpp remaps the
// CST816T coordinates to match. Override with -DLCD_ROTATION=<n> in build_flags.
// With LCD_AUTO_ROTATE=0 this is the fixed orientation. With auto-rotation
// on, it is the calibration offset: the orientation used while the IMU
// reports quadrant 0 (see rotation.h).
#ifndef LCD_ROTATION
#define LCD_ROTATION         3
#endif

// ---- IMU auto-rotation (QMI8658 accelerometer) ----
// 1 = follow the device's physical orientation, 0 = fixed LCD_ROTATION.
#ifndef LCD_AUTO_ROTATE
#define LCD_AUTO_ROTATE      1
#endif
// 1 if the IMU's quadrant counts the opposite way to the display rotation
// (turning the device makes the picture turn the wrong way).
#ifndef IMU_ROT_REVERSE
#define IMU_ROT_REVERSE      1
#endif
#define QMI8658_ADDR         0x6B

// ---- SPI display pins (ST7789, 4-wire SPI) ----
#define LCD_CS               21
#define LCD_SCLK             38
#define LCD_MOSI             39
#define LCD_DC               45
#define LCD_RST              40
#define LCD_BL               46    // backlight, LEDC PWM (TFT has no brightness cmd)

// ---- I2C bus (touch + codec + IMU share one bus) ----
#define IIC_SDA              42
#define IIC_SCL              41

// ---- Touch (CST816T, minimal inline I2C reader) ----
#define TP_INT               48
#define TP_RST               47
#define CST816_ADDR          0x15

// ---- Battery (ADC divider, no I2C-readable PMU) ----
// VBAT → 3.0x divider → GPIO1. BAT_EN (GPIO2) is the power-hold line: must be
// driven HIGH early in board_init() or the board browns out on battery power.
#define BAT_EN               2
#define BAT_ADC_PIN          1
#define BAT_VOLT_DIVIDER     3.0f

// ---- Audio (ES8311 mono codec + speaker, I2S) ----
// Pins from Waveshare's factory ES8311 example. No mic path wired (the ES7210
// mic ADC is a separate chip) — DIN unused.
#define SND_I2S_MCLK         8
#define SND_I2S_BCLK         9
#define SND_I2S_WS           10     // LRCK
#define SND_I2S_DOUT         12     // ESP → ES8311 (speaker)
#define SND_I2S_DIN          -1     // unused
#define SND_PA_PIN           7      // power-amp enable (HIGH = on)
#define SND_SAMPLE_RATE      44100  // I2S rate the ding is synthesized at
#define SND_ES8311_ADDR      0x18

// ---- Buttons (active-LOW GPIOs, from the Waveshare button example) ----
#define BTN_BACK_GPIO        0     // BOOT — primary, Space (PTT)
#define BTN_FWD_GPIO         5     // secondary, Shift+Tab (mode toggle)
#define BTN_PWR_GPIO         4     // PWR-role button — screens/brightness/pairing;
                                   // hold 8s = power off (see power.cpp)

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 1
#define BOARD_HAS_ROTATION         LCD_AUTO_ROTATE
#define BOARD_HAS_IMU              LCD_AUTO_ROTATE  // QMI8658, used for auto-rotation
#define BOARD_HAS_BATTERY          1
#define BOARD_HAS_IO_EXPANDER      0
