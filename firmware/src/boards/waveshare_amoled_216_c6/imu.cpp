#include "../../hal/imu_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <SensorQMI8658.hpp>

// QMI8658 on the 2.16 carrier PCB drives auto-rotation. Same poll/hysteresis
// logic as the S3 AMOLED-2.16 port; the C6 applies the resulting quadrant via
// the CO5300 MADCTL register (display.cpp) instead of a CPU rotation strip,
// so it costs no RAM.

#define IMU_POLL_MS       100    // ~10 Hz
#define STABLE_TIME_MS    300    // orientation must hold this long before rotating
#define TILT_THRESHOLD    0.5f   // ~30° from axis (sin 30° ≈ 0.5)

static SensorQMI8658 imu;
static uint8_t  current_rotation   = 0;
static uint8_t  candidate_rotation = 0;
static uint32_t candidate_since    = 0;
static uint32_t last_poll_ms       = 0;
static bool     imu_ok             = false;

// Dead zone: the dominant axis must beat the other by 25 % before we commit,
// otherwise (near 45°, e.g. a tilted stand) the current quadrant is kept.
#define AXIS_MARGIN       1.25f

static uint8_t accel_to_rotation(float ax, float ay) {
    float abs_ax = fabsf(ax);
    float abs_ay = fabsf(ay);
    if (abs_ax < TILT_THRESHOLD && abs_ay < TILT_THRESHOLD) {
        return 255;  // ambiguous (face-up/down)
    }
    if (abs_ay > abs_ax * AXIS_MARGIN) return (ay > 0) ? 3 : 1;
    if (abs_ax > abs_ay * AXIS_MARGIN) return (ax > 0) ? 0 : 2;
    return 255;      // too close to a diagonal — keep what we have
}

void imu_hal_init(void) {
    if (!imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
        Serial.println("QMI8658 init failed");
        return;
    }
    Serial.println("QMI8658 init OK (auto-rotation via MADCTL)");
    imu.configAccelerometer(
        SensorQMI8658::ACC_RANGE_4G,
        SensorQMI8658::ACC_ODR_LOWPOWER_21Hz,
        SensorQMI8658::LPF_MODE_3);
    imu.enableAccelerometer();
    imu_ok = true;
}

void imu_hal_tick(void) {
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
        Serial.printf("Rotation: %d (ax=%.2f ay=%.2f)\n", current_rotation, ax, ay);
    }
}

uint8_t imu_hal_rotation_quadrant(void) { return current_rotation; }
