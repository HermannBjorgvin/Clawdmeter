#include "../../hal/imu_hal.h"

// QMI8658C is populated (I2C 0x6B) but unused: the panel is fixed portrait.

void    imu_hal_init(void) {}
void    imu_hal_tick(void) {}
uint8_t imu_hal_rotation_quadrant(void) { return 0; }
