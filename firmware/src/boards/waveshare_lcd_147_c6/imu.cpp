#include "../../hal/imu_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <SensorQMI8658.hpp>

// QMI8658 confirmed present (I2C scan ACKed 0x6B, the library's default
// QMI8658_L_SLAVE_ADDRESS) but rotation is disabled — same reasoning as the
// other C6 ports: no PSRAM headroom for the rotation strip, and this kit's
// panel is only run at one fixed orientation anyway. Still initialized to
// keep the shared I2C bus healthy. Always reports quadrant 0.

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
