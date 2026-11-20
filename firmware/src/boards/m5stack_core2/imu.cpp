#include "../../hal/imu_hal.h"

// MPU6886 at 0x68, unused: fixed desk orientation.

void    imu_hal_init(void) {}
void    imu_hal_tick(void) {}
uint8_t imu_hal_rotation_quadrant(void) { return 0; }
