#include "../../hal/imu_hal.h"

// No IMU on this kit (the schematic populates none) and the panel orientation
// is fixed at boot, so rotation is off and these are pure stubs.

void    imu_hal_init(void) {}
void    imu_hal_tick(void) {}
uint8_t imu_hal_rotation_quadrant(void) { return 0; }
