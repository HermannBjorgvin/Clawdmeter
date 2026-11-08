#include "../../hal/imu_hal.h"

// QMI8658 is populated but unused: fixed orientation, no rotation.

void    imu_hal_init(void) {}
void    imu_hal_tick(void) {}
uint8_t imu_hal_rotation_quadrant(void) { return 0; }
