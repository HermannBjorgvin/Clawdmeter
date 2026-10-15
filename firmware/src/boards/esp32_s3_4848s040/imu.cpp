#include "../../hal/imu_hal.h"

// No IMU on this board; rotation is fixed at 0°.
void    imu_hal_init(void)              {}
void    imu_hal_tick(void)              {}
uint8_t imu_hal_rotation_quadrant(void) { return 0; }
