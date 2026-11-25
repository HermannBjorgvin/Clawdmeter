#include "../../hal/imu_hal.h"

// A QMI8658 is populated (0x6B), but the RGB panel has no CPU-rotation path
// in this port yet. Orientation is fixed; rotation stays at 0.

void    imu_hal_init(void) {}
void    imu_hal_tick(void) {}
uint8_t imu_hal_rotation_quadrant(void) { return 0; }
