#pragma once

// Waveshare ESP32-C6-Touch-AMOLED-1.8 — the C6 sibling of the S3 AMOLED-1.8.
//
// Same 368x448 SH8601 panel and FocalTech FT3168 (some units FT6146) touch
// as the S3 1.8 kit, but on the ESP32-C6 SoC: single-core RISC-V, no PSRAM,
// BLE 5 only, and its own GPIO map. AXP2101 PMU + QMI8658 IMU + a TCA9554 IO
// expander all share one I2C bus with the touch controller and RTC/codec.
//
// Unlike the S3 1.8 (where the TCA9554 gates display/touch *reset*), this
// board's expander gates display/touch *power*: P4 = display power, P5 =
// touch power, P7 = audio amp. The documented power-up sequence pulls P4/P5
// LOW, waits 200 ms, then drives them HIGH before the panel sees any QSPI.
//
// Pin map from the official product docs plus the third-party ESP-IDF BSP at
// github.com/chayuto/ESP32-C6-Touch-AMOLED-1.8 (CLAUDE.md GPIO map). The PWR
// button routes through the AXP2101 PKEY (like the C6 2.16), not the expander.

#define BOARD_NAME           "Waveshare AMOLED 1.8 (C6)"

// ---- Display geometry (portrait) ----
#define LCD_WIDTH            368
#define LCD_HEIGHT           448

// ---- QSPI display pins (SH8601) ----
#define LCD_CS               5
#define LCD_SCLK             0
#define LCD_SDIO0            1
#define LCD_SDIO1            2
#define LCD_SDIO2            3
#define LCD_SDIO3            4
// LCD reset is not wired to a MCU GPIO. The panel boots from its internal
// POR; the TCA9554 power-cycle (P4) below acts as the effective reset. The
// Arduino_GFX driver gets GFX_NOT_DEFINED for reset.

// ---- I2C bus (touch + PMU + IMU + IO expander + RTC + codec share one bus) ----
#define IIC_SDA              8
#define IIC_SCL              7

// ---- Touch (minimal inline I2C reader; FT3168/FT6146, FocalTech layout) ----
// Data layout at regs 0x02..0x06, same family as the S3 1.8 FT3168. INT only;
// reset is handled by the TCA9554 touch-power line, not a dedicated pin.
#define TP_INT               15
#define FT3168_ADDR          0x38

// ---- PMU ----
#define AXP2101_ADDR         0x34

// ---- IO expander (TCA9554/PCA9554 compatible) ----
// Gates display power, touch power, and the audio amp enable.
#define XCA9554_ADDR         0x20
#define IOX_PIN_LCD_PWR      4     // P4 → display power (active HIGH)
#define IOX_PIN_TP_PWR       5     // P5 → touch power   (active HIGH)
#define IOX_PIN_PA_EN        7     // P7 → audio amp enable (active HIGH)

// ---- Buttons ----
#define BTN_BACK_GPIO        9     // BOOT — primary, Space (PTT)
// PWR comes via the AXP2101 PKEY (see power.cpp); there is no secondary button.

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         0    // C6 has no PSRAM headroom for the rotation strip
#define BOARD_HAS_IMU              1    // present + initialized for I2C bus health
#define BOARD_HAS_BATTERY          1
#define BOARD_HAS_IO_EXPANDER      1
