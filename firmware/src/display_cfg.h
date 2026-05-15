#pragma once

#include <Arduino_GFX_Library.h>
#include <TouchDrvFT6X36.hpp>
#include <SensorQMI8658.hpp>
#include <Wire.h>

// ---- Display resolution ----
// Physical panel is portrait 368×448. We always render in landscape and
// rotate strips on the fly in my_flush_cb. LVGL/UI see the LOGICAL size.
#define PANEL_W     368   // physical panel width
#define PANEL_H     448   // physical panel height
#define LCD_WIDTH   448   // logical (landscape) width  — used by LVGL/UI
#define LCD_HEIGHT  368   // logical (landscape) height — used by LVGL/UI

// ---- QSPI display pins (SH8601) — from official schematic ----
#define LCD_CS      12
#define LCD_SCLK    11
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_TE      13   // tearing effect (optional, not wired in driver)

// LCD reset and panel power-enable are NOT on direct GPIOs — they live on
// the on-board TCA9554 I/O expander. The display will stay dark until we
// drive DSI_PWR_EN (EXIO1) high over I2C. See io_expander_init() in main.cpp.

// ---- Touch pins (FT3168 via shared I2C) ----
#define IIC_SDA     15
#define IIC_SCL     14
#define TP_INT      21
#define FT3168_ADDR 0x38

// ---- TCA9554 I/O expander (shared I2C) ----
// Drives LCD_RESET, DSI_PWR_EN, TP_RESET, SDCS, and a few system rails.
#define TCA9554_ADDR        0x20
#define EXIO_LCD_RESET      0   // P0
#define EXIO_LCD_PWR_EN     1   // P1  — MUST be high for display to power on
#define EXIO_TP_RESET       2   // P2

// ---- Global hardware objects (defined in main.cpp) ----
extern Arduino_DataBus *bus;
extern Arduino_SH8601 *gfx;
extern TouchDrvFT6X36 touch;
extern SensorQMI8658 imu;
