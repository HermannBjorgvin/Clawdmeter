#pragma once
#include <stdint.h>
#include "board.h"
#include "../../hal/imu_hal.h"

// Rotation the panel's MADCTL is set to right now (display.cpp). touch.cpp
// maps through this, not the IMU, so touch never runs ahead of the picture.
uint8_t lcd_applied_rotation(void);

// Board-private: the target panel rotation (0..3, Arduino_GFX convention) that
// display.cpp applies via MADCTL and touch.cpp mirrors for the CST816T.
// LCD_ROTATION is the base orientation; the IMU quadrant is added on top
// (or subtracted, with IMU_ROT_REVERSE) when auto-rotation is on.
static inline uint8_t lcd_effective_rotation(void) {
#if LCD_AUTO_ROTATE
    uint8_t q = imu_hal_rotation_quadrant() & 3;
#if IMU_ROT_REVERSE
    q = (4 - q) & 3;
#endif
    return (uint8_t)((LCD_ROTATION + q) & 3);
#else
    return (uint8_t)(LCD_ROTATION & 3);
#endif
}
