#include "../../hal/imu_hal.h"

// BMI270 is populated (0x68 on the internal bus) but the stick's enclosure
// mounts the panel at a fixed orientation, so rotation stays off and the
// IMU uninitialized. Candidate for shake gestures later.

void    imu_hal_init(void) {}
void    imu_hal_tick(void) {}
uint8_t imu_hal_rotation_quadrant(void) { return 0; }
