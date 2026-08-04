#include "../../hal/imu_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <SensorQMI8658.hpp>

// QMI8658 is populated on this board, but the panel mounts in a fixed
// orientation in the enclosure. We initialize the device so the shared I2C bus
// stays healthy, but always report rotation 0 (rotation disabled).

static SensorQMI8658 imu;

void imu_hal_init(void) {
    if (!imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
        Serial.println("QMI8658 init failed");
        return;
    }
    Serial.println("QMI8658 init OK (rotation disabled on this board)");
}

void imu_hal_tick(void) {
    // No-op — rotation is disabled.
}

uint8_t imu_hal_rotation_quadrant(void) { return 0; }
