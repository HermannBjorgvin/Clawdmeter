#include "../../hal/imu_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <SensorQMI8658.hpp>

// QMI8658 is populated on this board but rotation is disabled — the panel is
// round, so CPU-side rotation (used on the square/portrait boards) would be
// meaningless. Initialized anyway to keep the shared I2C bus healthy.

static SensorQMI8658 imu;

void imu_hal_init(void) {
    if (!imu.begin(Wire, QMI8658_ADDR, IIC_SDA, IIC_SCL)) {
        Serial.println("QMI8658 init failed");
        return;
    }
    Serial.println("QMI8658 init OK (rotation disabled — round panel)");
}

void imu_hal_tick(void) {
    // No-op — rotation is disabled.
}

uint8_t imu_hal_rotation_quadrant(void) { return 0; }
