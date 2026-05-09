#pragma once
#include <stdint.h>

void imu_init(void);
void imu_tick(void);            // call from loop(), handles auto-rotation
uint8_t imu_get_rotation(void); // current rotation 0-3
