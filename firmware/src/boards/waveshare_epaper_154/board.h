#pragma once

#define BOARD_NAME           "Waveshare ePaper 1.54"

// ---- Display geometry ----
#define LCD_WIDTH            200
#define LCD_HEIGHT           200

// ---- SSD1681 e-paper pins (SPI) ----
// Verified against the official Waveshare example at
// https://github.com/waveshareteam/ESP32-S3-ePaper-1.54
// (02_Example/Arduino/07_BATT_PWR_Test/user_config.h).
#define EPD_CS               11
#define EPD_SCLK             12
#define EPD_MOSI             13
#define EPD_DC               10
#define EPD_RST              9
#define EPD_BUSY             8
// EPD_PWR is active-LOW — drive LOW to enable the panel's onboard
// regulator, HIGH to power it off. Without this the SSD1681 has no Vcc
// and silently swallows every SPI transaction while the panel keeps
// showing whatever image it had at power-on (the factory clock demo,
// in our case).
#define EPD_PWR              6

// ---- Battery soft-power latch ----
// On battery, the PWR side-button only MOMENTARILY applies the battery
// rail. The MCU must immediately latch the rail ON by driving VBAT_PWR
// HIGH, or power collapses the instant the button is released — the MCU
// boots, writes one frame to the bistable e-paper, then dies, leaving a
// frozen splash with no animation. (On USB the board is powered by VBUS
// regardless, which is why it only fails on battery.)
//
// Verified against 07_BATT_PWR_Test/user_config.h + board_power_bsp.cpp:
//   VBAT_PWR_PIN = GPIO_NUM_17, configured OUTPUT, VBAT_POWER_ON() drives
//   it HIGH (active-HIGH, the OPPOSITE polarity of EPD_PWR). This must be
//   asserted before any display activity (board_init runs before display
//   init), matching the reference's user_app_init() ordering.
#define VBAT_PWR_GPIO        17
#define VBAT_PWR_ON_LEVEL    HIGH   // active-HIGH latch (per Waveshare BSP)

// ---- Buttons ----
#define BTN_BACK_GPIO        0     // BOOT — primary, Space (PTT)
#define BTN_PWR_GPIO         18    // PWR side-button (verified V2)

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         0
#define BOARD_HAS_IMU              0
#define BOARD_HAS_BATTERY          0
#define BOARD_HAS_IO_EXPANDER      0
