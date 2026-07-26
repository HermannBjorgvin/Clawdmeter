#include "../../hal/imu_hal.h"

// QMI8658 is populated on the kit (I2C 0x6B) but the enclosure mounts the
// panel at a fixed orientation, so rotation is off and the IMU stays
// uninitialized.

void    imu_hal_init(void) {}
void    imu_hal_tick(void) {}
uint8_t imu_hal_rotation_quadrant(void) { return 0; }
