#pragma once

// M5Stack FIRE — the first ESP32 *classic* port (all other boards are S3/C6)
// and the first with no touch panel: a 320x240 ILI9342C TFT over 4-wire SPI,
// three front push-buttons (A/B/C) in place of touch, an IP5306 power-bank PMU
// for battery reporting, and an MPU6886 IMU (populated, unused). 16 MB flash +
// PSRAM (WROVER module). USB is a CH9102 UART bridge — there is no native USB,
// so Serial is plain UART0 and ARDUINO_USB_CDC_ON_BOOT is NOT set in the env.
//
// Pin map is the canonical M5Stack Core/Fire wiring (M5GFX / M5Stack Arduino
// library): TFT on VSPI, buttons on the input-only GPIOs 37/38/39, internal
// I2C on 21/22 shared by the IP5306 and MPU6886.

#define BOARD_NAME           "M5Stack FIRE"

// ---- Display geometry (ILI9342C is natively 320x240 landscape) ----
#define LCD_WIDTH            320
#define LCD_HEIGHT           240

// ---- SPI display pins (ILI9342C, 4-wire SPI on VSPI) ----
#define LCD_CS               14
#define LCD_SCLK             18
#define LCD_MOSI             23
#define LCD_MISO             19    // wired but unused by the write-only HAL
#define LCD_DC               27
#define LCD_RST              33
#define LCD_BL               32    // backlight, LEDC PWM (TFT has no brightness cmd)

// ---- I2C bus (IP5306 PMU + MPU6886 IMU share the internal bus) ----
#define IIC_SDA              21
#define IIC_SCL              22

// ---- Power (IP5306 power-bank IC, I2C — 4-LED battery gauge, no fine %) ----
// The IP5306 owns hardware power on/off via M5Stack's dedicated red side
// button (double-tap on, long-hold off) — that path is entirely in silicon,
// so this port never synthesizes a software power-off (unlike the LCD-1.54).
#define IP5306_ADDR          0x75

// ---- Buttons (M5Stack front A/B/C, active-LOW, input-only GPIOs) ----
// GPIO 34..39 have no internal pull-ups; M5Stack fits external 10k pull-ups,
// so input.cpp uses plain INPUT (not INPUT_PULLUP).
//   A (left)   → primary,   HID Space (voice-mode PTT)
//   C (right)  → secondary, HID Shift+Tab (mode toggle)
//   B (middle) → PWR-role,  cycle screens / brightness / hold-to-pair
#define BTN_A_GPIO           39    // primary
#define BTN_B_GPIO           38    // PWR-role (owned by power.cpp)
#define BTN_C_GPIO           37    // secondary

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 1
#define BOARD_HAS_ROTATION         0    // fixed desk orientation
#define BOARD_HAS_IMU              0    // MPU6886 populated but unused
#define BOARD_HAS_BATTERY          1    // via IP5306
#define BOARD_HAS_TOUCH            0    // no touch panel — PWR button navigates the splash
#define BOARD_HAS_IO_EXPANDER      0
#define BOARD_HAS_SOUND            0    // speaker is a DAC path, not the I2S chime engine
