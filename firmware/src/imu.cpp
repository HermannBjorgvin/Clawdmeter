#include "imu.h"
#include "display_cfg.h"
#include <Arduino.h>
#include <math.h>

// QMI8658 accelerometer driver. The 1.8" AMOLED panel runs in landscape only —
// imu_get_rotation() returns 1 (90° CW) or 3 (270° CW), never portrait.
// When the device is held in portrait we hold the last landscape orientation.

#define IMU_POLL_MS       100    // read accel at ~10 Hz
#define STABLE_TIME_MS    300    // orientation must be stable this long before flipping
#define LANDSCAPE_THRESHOLD 0.6f // |ax| must exceed this (relative to 1g) to flip

static uint8_t  current_rotation = 1;    // boot in landscape (90° CW)
static uint8_t  candidate_rotation = 1;
static uint32_t candidate_since = 0;
static uint32_t last_poll_ms = 0;
static bool     imu_ok = false;

// Landscape-only orientation: ax dominant means the board is on its side.
// Returns 255 when the device is portrait or flat (ambiguous — keep current).
static uint8_t accel_to_rotation(float ax, float ay) {
    (void)ay;
    if (ax >  LANDSCAPE_THRESHOLD) return 1;  // one landscape direction
    if (ax < -LANDSCAPE_THRESHOLD) return 3;  // the other
    return 255;
}

void imu_init(void) {
    if (!imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
        Serial.println("QMI8658 init failed");
        return;
    }
    Serial.println("QMI8658 init OK");

    imu.configAccelerometer(
        SensorQMI8658::ACC_RANGE_4G,
        SensorQMI8658::ACC_ODR_LOWPOWER_21Hz,
        SensorQMI8658::LPF_MODE_3);
    imu.enableAccelerometer();

    imu_ok = true;
}

void imu_tick(void) {
    if (!imu_ok) return;

    uint32_t now = millis();
    if (now - last_poll_ms < IMU_POLL_MS) return;
    last_poll_ms = now;

    float ax, ay, az;
    if (!imu.getAccelerometer(ax, ay, az)) return;

    uint8_t target = accel_to_rotation(ax, ay);
    if (target == 255 || target == current_rotation) {
        candidate_rotation = current_rotation;
        return;
    }

    if (target != candidate_rotation) {
        candidate_rotation = target;
        candidate_since = now;
    } else if (now - candidate_since >= STABLE_TIME_MS) {
        current_rotation = target;
        Serial.printf("Rotation: %d\n", current_rotation);
    }
}

uint8_t imu_get_rotation(void) {
    return current_rotation;
}
