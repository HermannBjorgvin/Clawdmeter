#pragma once
#include "../../clawd_prefs.h"
#include <stdint.h>
#include "board.h"
#include "../../hal/imu_hal.h"

// Rotation the panel's MADCTL is set to right now (display.cpp). touch.cpp
// maps through this, not the IMU, so touch never runs ahead of the picture.
uint8_t lcd_applied_rotation(void);

// Board-private: the target panel rotation (0..3, Arduino_GFX convention) that
// display.cpp applies via MADCTL and touch.cpp mirrors for the CST816T.
// With auto-rotation off it is LCD_ROTATION, as on earlier firmware. With it
// on, LCD_IMU_OFFSET (the panel rotation for IMU quadrant 0; upright is quadrant 3)
// plus the quadrant, subtracted with IMU_ROT_REVERSE. A fixed `ro` arrives as
// a quadrant too (imu_hal_fixed_quadrant in imu.cpp undoes the reversal), so
// fixed values turn clockwise.
static inline uint8_t lcd_effective_rotation(void) {
#if LCD_AUTO_ROTATE
    uint8_t q = display_rotation_quadrant() & 3;
#if IMU_ROT_REVERSE
    q = (4 - q) & 3;
#endif
    return (uint8_t)((LCD_IMU_OFFSET + q) & 3);
#else
    return (uint8_t)(LCD_ROTATION & 3);
#endif
}
