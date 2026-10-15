#pragma once

// Waveshare ESP32-S3-Touch-LCD-1.47 — 1.47" 172x320 IPS TFT kit, driven here
// in LANDSCAPE (320x172). JD9853 panel over plain 4-wire SPI (the JD9853 is
// register-compatible enough with the ST7789 command set that Arduino_GFX's
// Arduino_ST7789 drives it once the vendor init sequence has been pushed —
// that's exactly what Waveshare's own Arduino demo does) + AXS5106L capacitive
// touch + a plain VBAT divider (no PMU, no IO expander, no codec, no IMU).
//
// Pin map verified against the official schematic
// (files.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.47/ESP32-S3-Touch-LCD-1.47-Schematic.pdf):
//   IO21 LCD_CS   IO38 LCD_SCL  IO39 LCD_SDA  IO40 LCD_RST  IO45 LCD_DC  IO46 LCD_BL
//   IO41 TP_SCL   IO42 TP_SDA   IO47 TP_RST   IO48 TP_INT   IO12 BAT_ADC
// (Waveshare's own ESP-IDF BSP header has TP_RST/TP_INT swapped; the schematic
// and their Arduino demo agree with the values below.)

#define BOARD_NAME           "Waveshare LCD 1.47"

// ---- Display geometry ----
// Panel is physically 172x320 portrait; the firmware runs it rotated so the
// UI gets a 320x172 landscape canvas. LCD_WIDTH/HEIGHT are post-rotation
// (that's what BoardCaps and the UI layout consume).
#define LCD_WIDTH            320
#define LCD_HEIGHT           172
#define LCD_PANEL_W          172   // native, pre-rotation
#define LCD_PANEL_H          320
#define LCD_ROTATION         1     // 1 = landscape (USB-C on the left)
// The 172-wide active area starts at column 34 of the JD9853's 240-wide GRAM.
// Arduino_TFT::setRotation maps this offset onto the correct axis per rotation.
#define LCD_COL_OFFSET       34

// ---- SPI display pins (JD9853, 4-wire SPI) ----
#define LCD_CS               21
#define LCD_SCLK             38
#define LCD_MOSI             39
#define LCD_DC               45
#define LCD_RST              40
#define LCD_BL               46    // backlight, LEDC PWM (TFT has no brightness cmd)

// ---- I2C bus (touch only — nothing else is populated on this kit) ----
#define IIC_SDA              42
#define IIC_SCL              41

// ---- Touch (AXS5106L, minimal inline I2C reader) ----
#define TP_INT               48
#define TP_RST               47
#define AXS5106L_ADDR        0x63

// ---- Battery (ADC divider, no I2C-readable PMU) ----
// VBAT → 3.0x divider → GPIO12 (ADC2_CH1). There is NO power-hold latch on
// this board (unlike the LCD-1.54's BAT_EN), so nothing to assert at boot.
#define BAT_ADC_PIN          12
#define BAT_VOLT_DIVIDER     3.0f

// ---- Buttons ----
// The kit ships exactly two keys: BOOT (GPIO 0) and RESET (tied to EN, not
// readable). So there is only ONE usable user button, and it is given the
// PWR role — screens / brightness / the hold-3s-release pairing gesture —
// because without it the device could never be paired at all. The HID
// push-to-talk (Space) and mode-toggle (Shift+Tab) buttons the bigger boards
// have simply don't exist here; input.cpp reports no buttons and
// BoardCaps.button_count is 0. Screen switching is also available by tapping
// the touchscreen.
#define BTN_PWR_GPIO         0     // BOOT — PWR role (screens/brightness; hold ~3s + release to pair)

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         0    // panel is rotated once at init, not at runtime
#define BOARD_HAS_IMU              0    // no IMU populated
#define BOARD_HAS_BATTERY          1
#define BOARD_HAS_IO_EXPANDER      0
#define BOARD_HAS_SOUND            0    // no codec, no buzzer
