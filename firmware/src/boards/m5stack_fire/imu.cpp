#include "../../hal/imu_hal.h"

// An MPU6886 is populated on the M5Stack FIRE (I2C 0x68), but this port runs at
// a fixed desk orientation, so rotation is off and the IMU stays uninitialized
// — matching the AMOLED-1.8 / LCD-1.54 posture.

void    imu_hal_init(void) {}
void    imu_hal_tick(void) {}
uint8_t imu_hal_rotation_quadrant(void) { return 0; }
