#pragma once

// Waveshare ESP32-S3-Touch-LCD-1.85B — round LCD, 360x360, CNC aluminum case.
// ST77916 QSPI TFT + CST816S touch + BQ27220 fuel gauge + QMI8658 IMU.
// No AXP2101 PMU and no secondary/PWR button on this board — only BOOT
// (GPIO0) and a hardware RESET exist. Pins verified against Waveshare's
// official example repo (waveshareteam/ESP32-S3-Touch-LCD-1.85B), not yet
// verified on physical hardware in this repo.

#define BOARD_NAME           "Waveshare LCD 1.85B"

// ---- Display geometry (round, but framebuffer is square) ----
#define LCD_WIDTH            360
#define LCD_HEIGHT           360

// ---- QSPI display pins (ST77916) ----
#define LCD_CS               21
#define LCD_SCLK             40
#define LCD_SDIO0            46
#define LCD_SDIO1            45
#define LCD_SDIO2            42
#define LCD_SDIO3            41
#define LCD_RESET            3     // direct GPIO, active low — no IO expander on this board

// ---- Backlight (TFT needs a physical PWM backlight, unlike the AMOLED boards) ----
#define LCD_BACKLIGHT_PIN    5
#define LCD_BACKLIGHT_ON_LEVEL 1

// ---- I2C bus (touch + IMU + fuel gauge + RTC all share one bus) ----
#define IIC_SDA              11
#define IIC_SCL              10

// ---- Touch (CST816S, direct-GPIO reset — no IO expander) ----
#define TP_INT               4
#define TP_RST               1
#define CST816_ADDR          0x15

// ---- IMU ----
#define QMI8658_ADDR         0x6B

// ---- Fuel gauge (replaces the AXP2101 PMU used on the AMOLED boards) ----
#define BQ27220_ADDR         0x55

// ---- Buttons ----
#define BTN_BACK_GPIO        0     // BOOT — primary, Space (PTT)
// No secondary/PWR button on this board's stock hardware: Waveshare's docs
// and demo firmware list only BOOT + a hardware RESET. Splash-cycling,
// brightness-cycling, and the hold-to-pair gesture (all normally driven by
// a PWR button) are unavailable here — see power.cpp.

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         0   // round panel — CPU rotation would be meaningless
#define BOARD_HAS_IMU              1
#define BOARD_HAS_BATTERY          1   // via BQ27220, not a PMU
#define BOARD_HAS_IO_EXPANDER      0
#define BOARD_HAS_SOUND            0   // stock hardware has an I2S codec + speaker, not
                                        // the simple LEDC piezo buzzer this HAL targets
