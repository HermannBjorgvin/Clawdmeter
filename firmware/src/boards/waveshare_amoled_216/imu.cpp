#include "../../hal/imu_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <ImuDrv.hpp>

// Poll and hysteresis timing
#define IMU_POLL_MS       100    // ~10 Hz
#define STABLE_TIME_MS    300    // orientation must hold this long before rotating
#define TILT_THRESHOLD    0.5f   // ~30° from axis (sin 30° ≈ 0.5)

static SensorQMI8658 imu;
static uint8_t  current_rotation   = 0;
static uint8_t  candidate_rotation = 0;
static uint32_t candidate_since    = 0;
static uint32_t last_poll_ms       = 0;
static bool     imu_ok             = false;
static float    accel_scale        = 0;      // g per LSB, set by configAccel()

static uint8_t accel_to_rotation(float ax, float ay) {
    float abs_ax = fabsf(ax);
    float abs_ay = fabsf(ay);
    if (abs_ax < TILT_THRESHOLD && abs_ay < TILT_THRESHOLD) {
        return 255;  // ambiguous (face-up/down)
    }
    if (abs_ay > abs_ax) return (ay > 0) ? 3 : 1;
    return (ax > 0) ? 0 : 2;
}

// SensorLib 0.5's begin() also fails when the QMI8658's USID handshake
// times out, which 0.2.6 ignored; a failed begin() clears its own state,
// so a short bounded retry is safe.
#define IMU_BEGIN_TRIES   3

void imu_hal_init(void) {
    bool found = false;
    for (int i = 0; i < IMU_BEGIN_TRIES && !found; i++) {
        if (i) delay(10);
        // No pins: board_init() already started Wire, and SensorLib would
        // call Wire.setPins() on the running bus (an [E] log line).
        found = imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS);
    }
    if (!found) {
        Serial.println("QMI8658 init failed");
        return;
    }
    // SensorLib 0.5 has no low-power ODR for plain reads; 28 Hz is the
    // lowest normal-mode rate, still well above the 10 Hz poll.
    if (!imu.configAccel(AccelFullScaleRange::FS_4G, 28.0f,
                         SensorQMI8658::LpfMode::MODE_3) ||
        !imu.enableAccel()) {
        Serial.println("QMI8658 accel config failed");
        return;
    }
    accel_scale = imu.getAccelScale();
    imu_ok = true;
    Serial.println("QMI8658 init OK");
}

void imu_hal_tick(void) {
    if (!imu_ok) return;
    uint32_t now = millis();
    if (now - last_poll_ms < IMU_POLL_MS) return;
    last_poll_ms = now;

    int16_t rx, ry, rz;
    if (!imu.readAccelRaw(rx, ry, rz)) return;
    float ax = rx * accel_scale;  // g
    float ay = ry * accel_scale;

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

uint8_t imu_hal_rotation_quadrant(void) { return current_rotation; }
