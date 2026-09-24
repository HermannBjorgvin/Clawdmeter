#include "../../hal/imu_hal.h"

// No IMU on this board — fixed orientation, chosen at compile time by the
// portrait/landscape env.
void imu_hal_init(void) {}
void imu_hal_tick(void) {}
uint8_t imu_hal_rotation_quadrant(void) { return 0; }
