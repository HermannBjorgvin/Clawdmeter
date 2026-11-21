#pragma once

// M5Stack Core2: classic ESP32 (D0WDQ6-V3), 16 MB flash + 8 MB PSRAM, the same
// 320x240 ILI9342C panel as the FIRE, but:
//   - the panel's power, reset and backlight hang off the power chip, not GPIOs;
//   - no physical front buttons: an FT6336U touch panel covers the screen
//     *and* a 40 px strip below it, where three printed circles act as
//     buttons A / B / C (ft6336.cpp splits the two zones);
//   - two power chips by revision: AXP192 (v1.0) or AXP2101 (v1.1), detected
//     at boot (pmu.cpp).
// USB is a UART bridge (CP2104 on v1.0, CH9102 on v1.1): no native USB, so the
// env must not set ARDUINO_USB_CDC_ON_BOOT.

#define BOARD_NAME           "M5Stack Core2"

// ---- Display geometry (ILI9342C is natively 320x240 landscape) ----
#define LCD_WIDTH            320
#define LCD_HEIGHT           240

// ---- SPI display pins (ILI9342C on VSPI, shared with the microSD slot) ----
#define LCD_CS               5
#define LCD_SCLK             18
#define LCD_MOSI             23
#define LCD_MISO             38
#define LCD_DC               15
// Reset and backlight are power-chip outputs (pmu.cpp): no GPIOs.
#define SD_CS                4     // held high so the card never answers the panel's traffic

// ---- I2C bus (power chip 0x34, touch 0x38, IMU 0x68, RTC 0x51) ----
#define IIC_SDA              21
#define IIC_SCL              22
#define PMU_ADDR             0x34
#define TOUCH_ADDR           0x38
#define TOUCH_INT_GPIO       39

// ---- Speaker: NS4168 I2S amplifier, no codec (chime.h's es8311_addr 0) ----
// The amp's enable is a power-chip output: ALDO3 (v1.1) or AXP192 GPIO2 (v1.0).
#define SND_I2S_BCLK         12
#define SND_I2S_WS           0
#define SND_I2S_DOUT         2
#define SND_SAMPLE_RATE      22050 // the classic ESP32 takes ~10 s to render the ding at 44.1 kHz
#define SND_VOLUME           100   // % of the ding's level (no codec volume)

// ---- Touch-button strip below the panel ----
// The touch panel reports y up to ~279; y >= LCD_HEIGHT is the button strip.
// Thirds of the width: A (left) = primary, B (middle) = PWR role, C (right) = secondary.
#define TOUCH_STRIP_Y        LCD_HEIGHT

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 1
#define BOARD_HAS_ROTATION         0    // fixed desk orientation
#define BOARD_HAS_IMU              0    // MPU6886 populated but unused
#define BOARD_HAS_BATTERY          1    // via the power chip
#define BOARD_HAS_IO_EXPANDER      0
