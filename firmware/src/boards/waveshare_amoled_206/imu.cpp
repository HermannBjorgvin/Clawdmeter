#include "../../hal/imu_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <ImuDrv.hpp>

// QMI8658 is present on this board, but rotation is disabled in the
// watch enclosure. We still initialize the chip so the I2C bus is
// healthy and future features (step counting, tap detect) can use it.

static SensorQMI8658 imu;
static bool imu_ok = false;

void imu_hal_init(void) {
    // No pins: board_init() already started Wire, and SensorLib would
    // call Wire.setPins() on the running bus (an [E] log line).
    if (!imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS)) {
        Serial.println("QMI8658 init failed");
        return;
    }
    Serial.println("QMI8658 init OK");
    imu_ok = true;
}

void imu_hal_tick(void) {
    (void)imu_ok;  // nothing to poll while rotation is disabled
}

uint8_t imu_hal_rotation_quadrant(void) { return 0; }
