#include "../../hal/imu_hal.h"

// No IMU on this board — fixed orientation.

void    imu_hal_init(void) {}
void    imu_hal_tick(void) {}
uint8_t imu_hal_rotation_quadrant(void) { return 0; }
