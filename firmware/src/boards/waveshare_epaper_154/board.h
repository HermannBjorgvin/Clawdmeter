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

// ---- Battery voltage sense (ADC) ----
// GPIO4 = ESP32-S3 ADC1_CHANNEL_3, behind a 1:2 hardware divider, so the
// firmware reconstructs the real pack voltage as (measured pin mV * 2).
// Verified against Waveshare 02_Example/Arduino/01_ADC_Test/adc_bsp.cpp,
// which reads ADC_UNIT_1/ADC_CHANNEL_3 at ADC_ATTEN_DB_12 and computes
// `vol * 2`. GPIO4 is in the ADC1 bank (GPIO1..10), so it is radio-safe to
// read while NimBLE is active (ADC2 on GPIO11..20 is not). GPIO4 is unused
// elsewhere on this board. This is a SEPARATE net from VBAT_PWR (GPIO17),
// which is the power-enable latch above, not a sense line.
#define VBAT_ADC_GPIO        4
#define VBAT_ADC_DIVIDER     2     // 1:2 divider — reconstruct Vbat = pin_mV * divider

// ---- Buttons ----
#define BTN_BACK_GPIO        0     // BOOT — primary, Space (PTT)
#define BTN_PWR_GPIO         18    // PWR side-button (verified V2)

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         0
#define BOARD_HAS_IMU              0
#define BOARD_HAS_BATTERY          1
#define BOARD_HAS_IO_EXPANDER      0
