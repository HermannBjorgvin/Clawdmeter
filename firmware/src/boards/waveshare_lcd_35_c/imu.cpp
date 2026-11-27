#include "../../hal/imu_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <ImuDrv.hpp>

// Poll and hysteresis timing
#define IMU_POLL_MS       100    // ~10 Hz
#define STABLE_TIME_MS    300    // orientation must hold this long before flipping
#define TILT_THRESHOLD    0.5f   // ~30 degrees off the panel's horizontal (sin 30 ~ 0.5)

// Only 0 (upright) and 2 (upside down) are reported: the panel flips in
// hardware and stays 320x480. Flat or ambiguous keeps the current value.
static SensorQMI8658 imu;
static uint8_t  current_rotation   = 0;
static uint8_t  candidate_rotation = 0;
static uint32_t candidate_since    = 0;
static uint32_t last_poll_ms       = 0;
static bool     imu_ok             = false;

// Panel's vertical is the accelerometer X axis: upright reads ax ~ -1 g and
// upside down reads ax ~ +1 g (measured on this board).
static uint8_t accel_to_rotation(float ax) {
    if (ax < -TILT_THRESHOLD) return 0;
    if (ax > TILT_THRESHOLD)  return 2;
    return 255;  // ambiguous
}

void imu_hal_init(void) {
    if (!imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
        Serial.println("QMI8658 init failed");
        return;
    }
    Serial.println("QMI8658 init OK");
    // SensorLib 0.5 has no low-power ODR for plain reads; 28 Hz is the
    // lowest normal-mode rate, still well above the 10 Hz poll.
    imu.configAccel(AccelFullScaleRange::FS_4G, 28.0f,
                    SensorQMI8658::LpfMode::MODE_3);
    imu.enableAccel();
    imu_ok = true;
}

void imu_hal_tick(void) {
    if (!imu_ok) return;
    uint32_t now = millis();
    if (now - last_poll_ms < IMU_POLL_MS) return;
    last_poll_ms = now;

    int16_t rx, ry, rz;
    if (!imu.readAccelRaw(rx, ry, rz)) return;
    float ax = rx * imu.getAccelScale();  // g

    uint8_t target = accel_to_rotation(ax);
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
