#include "../../hal/imu_hal.h"

// No IMU on this template. If your board ships an accelerometer (e.g.
// QMI8658 + want auto-rotation), copy boards/waveshare_amoled_216/imu.cpp
// here and set BOARD_HAS_ROTATION=1 in board.h.

void    imu_hal_init(void) {}
void    imu_hal_tick(void) {}
uint8_t imu_hal_rotation_quadrant(void) { return 0; }
