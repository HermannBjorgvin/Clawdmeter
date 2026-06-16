#pragma once

// Waveshare ESP32-S3-Touch-AMOLED-1.43 — 466x466 round AMOLED.
// CO5300 (or SH8601 on early units) + CST820/FT3168 touch + QMI8658 IMU + RTC.
// 16 MB flash, 8 MB octal PSRAM. No IO expander, no AXP2101 PMU, no PWR button.
//
// Pins below verified against a working PlatformIO port of this exact board
// (lcd_config.h: CS=9 PCLK=10 D0..D3=11..14 RST=21, touch I2C SDA=47 SCL=48).
// The display RST is a direct GPIO here (the 1.8 routed it through an expander),
// so the GFX driver owns reset and board_init() needs no expander step.
//
// Two panel revisions exist, same as the 1.8 line — check the label on the back:
//   - early:   SH8601 + FT3168 (touch @ 0x38)
//   - current: CO5300 + CST820 (touch @ 0x15)   <-- what Waveshare ships now
// board_init.cpp auto-detects by which touch address answers; display.cpp
// picks the matching GFX driver. If yours is unambiguous you can hard-wire it.

#define BOARD_NAME           "Waveshare AMOLED 1.43"

// ---- Display geometry (square framebuffer; panel is a 466 round cut) ----
#define LCD_WIDTH            466
#define LCD_HEIGHT           466

// ---- QSPI display pins (CO5300 / SH8601) ----
#define LCD_CS               9
#define LCD_SCLK             10
#define LCD_SDIO0            11
#define LCD_SDIO1            12
#define LCD_SDIO2            13
#define LCD_SDIO3            14
#define LCD_RESET            21       // direct GPIO — driver drives reset itself

// ---- I2C bus (touch only — IMU/RTC live on a separate bus we don't use) ----
#define IIC_SDA              47
#define IIC_SCL              48

// ---- Touch ----
// FocalTech-style data layout at regs 0x02..0x06 on both controllers; only the
// address differs. CST820 (0x15) is Hynitron CST8xx — same reader as CST816.
#define FT3168_ADDR          0x38     // early (SH8601) revision
#define CST816_ADDR          0x15     // current (CO5300) revision — CST820 shares this
// No touch INT pin wired in this port — we poll in touch_hal_read() (a 5-byte
// I2C burst is ~150us @ 400kHz, well inside LVGL's per-refresh touch budget).
// If you bring the INT line out to a GPIO, define TP_INT and switch to the
// interrupt path (see commented block in touch.cpp).

// ---- Buttons ----
#define BTN_BACK_GPIO        0        // BOOT — primary, Space (PTT)
// No dedicated PWR button on this board (only RST, which hard-resets the MCU).
// power.cpp synthesizes the hold-3s-to-pair gesture from a long-press of BOOT.

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 0  // no second HID button (no Shift+Tab)
#define BOARD_HAS_ROTATION         0  // round, fixed orientation
#define BOARD_HAS_IMU              0  // QMI8658 present but unused (no rotation)
#define BOARD_HAS_BATTERY          0  // no PMU/fuel gauge — hides battery indicator
#define BOARD_HAS_IO_EXPANDER      0  // reset lines are direct GPIO
