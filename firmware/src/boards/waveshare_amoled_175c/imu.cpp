#include "../../hal/imu_hal.h"

// The fitted IMU is not used by this port.

void    imu_hal_init(void) {}
void    imu_hal_tick(void) {}
uint8_t imu_hal_rotation_quadrant(void) { return 0; }
