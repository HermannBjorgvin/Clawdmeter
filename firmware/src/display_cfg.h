#pragma once
//
// Central board dispatcher.
//
// Each supported board defines BOARD_<NAME> via a build flag in
// platformio.ini, e.g.:
//      build_flags = -DBOARD_WAVESHARE_AMOLED_216
//
// This file selects the matching board header (pinout, dimensions,
// capability flags), pulls in the third-party driver headers that the
// selected hardware needs, and declares the shared `gfx` graphics object.
// Capability flags (BOARD_HAS_TOUCH / BOARD_HAS_PMU / BOARD_HAS_IMU /
// BOARD_HAS_BATTERY_ADC) gate the optional subsystems in main.cpp,
// power.cpp, imu.cpp, touch handling, etc.
//

#include <Arduino_GFX_Library.h>

#if defined(BOARD_WAVESHARE_AMOLED_216)
#  include "boards/board_waveshare_amoled_216.h"
#elif defined(BOARD_LILYGO_T_DISPLAY_S3)
#  include "boards/board_lilygo_t_display_s3.h"
#else
#  error "No board selected. Define one of: BOARD_WAVESHARE_AMOLED_216, BOARD_LILYGO_T_DISPLAY_S3."
#endif

// ---- Legacy aliases ----
// The original code referenced LCD_WIDTH / LCD_HEIGHT; keep them as
// synonyms for the new BOARD_LCD_W / BOARD_LCD_H so we don't churn every
// callsite when adding boards.
#define LCD_WIDTH   BOARD_LCD_W
#define LCD_HEIGHT  BOARD_LCD_H

// ---- I2C bus presence ----
// Touch, PMU, and IMU all live on the same I2C bus on Waveshare; on
// LILYGO none of them are present. Reduce to a single capability flag so
// main.cpp can skip Wire.begin() entirely on boards that don't need it.
#define BOARD_HAS_I2C \
    (BOARD_HAS_TOUCH || BOARD_HAS_PMU || BOARD_HAS_IMU)

// ---- Driver headers (only what the selected board needs) ----
#if BOARD_HAS_I2C
#  include <Wire.h>
#endif
#if BOARD_HAS_TOUCH
#  include <TouchDrvCSTXXX.hpp>
#endif
#if BOARD_HAS_PMU
#  include <XPowersLib.h>
#endif
#if BOARD_HAS_IMU
#  include <SensorQMI8658.hpp>
#endif

// ---- Global hardware objects (defined in main.cpp) ----
// `gfx` is the base GFX pointer so callsites stay board-agnostic. The
// concrete driver type (Arduino_CO5300 vs Arduino_ST7789) is picked in
// main.cpp behind the BOARD_* macro.
extern Arduino_GFX *gfx;
#if BOARD_HAS_TOUCH
extern TouchDrvCST92xx touch;
#endif
#if BOARD_HAS_PMU
extern XPowersPMU pmu;
#endif
#if BOARD_HAS_IMU
extern SensorQMI8658 imu;
#endif

// ---- Board-level helpers (implemented in main.cpp / power.cpp) ----
// Set panel brightness 0..255. On AMOLED boards this drives the CO5300
// DCS brightness command; on backlit LCDs it maps to the backlight PWM
// (or simply ON/OFF if PWM isn't wired up). No-op on boards without
// brightness control.
void board_set_brightness(uint8_t level);
