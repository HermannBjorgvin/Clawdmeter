#pragma once

// ESP32 DevKit v1 (WROOM-32) + ST7789V2 1.69" 240x280 over plain 4-wire SPI.
//
// DIY wiring, not a vendor kit: display only (no touch, no PMU, no battery,
// no codec) plus the on-board BOOT button and two buttons you wire to GND.
//
// First classic-ESP32 port in the tree — every other board is an S3 or a C6.
// Two consequences the envs above don't have:
//   * No PSRAM. Shared code gates on BOARD_HAS_PSRAM: LVGL strips shrink to
//     20 lines, the splash renders into a 60x60 buffer LVGL upscales, the
//     corner mascot falls back to the static clawd_still art, and the
//     `screenshot` serial command is compiled out.
//   * No native USB. Serial is UART0 through the board's CP2102/CH340, so
//     ARDUINO_USB_CDC_ON_BOOT must stay unset (see the env in platformio.ini).

#define BOARD_NAME           "ESP32 DevKit ST7789"

// ---- Display geometry ----
// 1.69" ST7789V2. If you wired a different panel of the same family, change
// these two plus the offsets in display.cpp (240x240 -> 0,0 / 240x320 -> 0,0).
#define LCD_WIDTH            240
#define LCD_HEIGHT           280

// ---- Display pins (4-wire SPI, VSPI defaults where possible) ----
// GPIO 6..11 are the flash bus, 34..39 are input-only, 1/3 are UART0, and
// 0/2/12/15 are strapping pins — all avoided here.
#define LCD_SCLK             18    // VSPI SCK  -> panel SCL/CLK
#define LCD_MOSI             23    // VSPI MOSI -> panel SDA/DIN
#define LCD_CS               5     // panel CS  (tie to GND on modules with no CS pin)
#define LCD_DC               16    // panel DC/RS
#define LCD_RST              17    // panel RES/RST
#define LCD_BL               4     // backlight, LEDC PWM (ST7789 has no brightness cmd)

// ---- Buttons (all active-LOW to GND, internal pull-ups) ----
#define BTN_BACK_GPIO        0     // on-board BOOT — primary, Space (PTT)
#define BTN_FWD_GPIO         25    // secondary, Shift+Tab (mode toggle)
#define BTN_PWR_GPIO         26    // PWR role — cycle screens / brightness / hold-to-pair

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 1
#define BOARD_HAS_ROTATION         0
#define BOARD_HAS_IMU              0
#define BOARD_HAS_BATTERY          0
#define BOARD_HAS_IO_EXPANDER      0
#define BOARD_HAS_SOUND            0
#define BOARD_HAS_TOUCH            0   // display-only module — PWR toggles screens
