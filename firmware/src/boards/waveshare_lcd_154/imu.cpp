#include "../../hal/imu_hal.h"
#include "board.h"
#include <Arduino.h>

#if LCD_AUTO_ROTATE

#include <Wire.h>
#include <ImuDrv.hpp>

// QMI8658 accelerometer → orientation quadrant, same scheme as the
// AMOLED-2.16 port: poll at ~10 Hz, ignore face-up/face-down, and only
// switch once a new orientation has held for STABLE_TIME_MS.

#define IMU_POLL_MS       100    // ~10 Hz
#define STABLE_TIME_MS    400    // orientation must hold this long before rotating
#define TILT_THRESHOLD    0.5f   // ~30° from axis (sin 30° ≈ 0.5)
#define IMU_BEGIN_TRIES   3

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
        return 255;  // ambiguous (lying flat)
    }
    if (abs_ay > abs_ax) return (ay > 0) ? 3 : 1;
    return (ax > 0) ? 0 : 2;
}

// Read one accelerometer sample in g and reject implausible ones. Right after
// enableAccel() the QMI8658 returns a stale register block (observed: all
// three axes -0.83 g, |a| = 1.44 g) and then its low-pass filter settles
// slowly over ~1 s (observed 0.60 -> 0.76 -> 1.00 g on the gravity axis).
// So there is no reliable instant boot reading: the orientation is left to
// imu_hal_tick(), whose STABLE_TIME_MS hold rides out the settling. A device
// at rest reads ~1 g; the window is wide enough for normal handling.
#define ACCEL_MIN_G2  (0.5f * 0.5f)
#define ACCEL_MAX_G2  (1.3f * 1.3f)

static bool read_accel(float* ax, float* ay, float* az) {
    int16_t rx, ry, rz;
    if (!imu.readAccelRaw(rx, ry, rz)) return false;
    if (rx == ry && ry == rz) return false;          // stale / uninitialized block
    *ax = rx * accel_scale;
    *ay = ry * accel_scale;
    *az = rz * accel_scale;
    float m2 = *ax * *ax + *ay * *ay + *az * *az;
    return m2 >= ACCEL_MIN_G2 && m2 <= ACCEL_MAX_G2;
}

void imu_hal_init(void) {
    bool found = false;
    for (int i = 0; i < IMU_BEGIN_TRIES && !found; i++) {
        if (i) delay(10);
        // No pins: board_init() already started Wire (touch + codec share it).
        found = imu.begin(Wire, QMI8658_ADDR);
    }
    if (!found) {
        Serial.println("QMI8658 init failed - auto-rotation off");
        return;
    }
    if (!imu.configAccel(AccelFullScaleRange::FS_4G, 28.0f,
                         SensorQMI8658::LpfMode::MODE_3) ||
        !imu.enableAccel()) {
        Serial.println("QMI8658 accel config failed - auto-rotation off");
        return;
    }
    accel_scale = imu.getAccelScale();
    imu_ok = true;
    Serial.println("QMI8658 init OK");

    // No boot snap (see read_accel): start at quadrant 0 = LCD_ROTATION and
    // let the first stable tick reading rotate into place.
}

void imu_hal_tick(void) {
    if (!imu_ok) return;
    uint32_t now = millis();
    if (now - last_poll_ms < IMU_POLL_MS) return;
    last_poll_ms = now;

    float ax, ay, az;  // g
    if (!read_accel(&ax, &ay, &az)) return;   // also drops shakes / bumps

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
        // Raw axes logged so the mounting (LCD_ROTATION / IMU_ROT_REVERSE)
        // can be calibrated from the serial monitor.
        Serial.printf("Rotation: %d (ax=%.2f ay=%.2f az=%.2f)\n", current_rotation,
                      ax, ay, az);
    }
}

uint8_t imu_hal_rotation_quadrant(void) { return current_rotation; }

#else  // !LCD_AUTO_ROTATE

// Fixed orientation: the QMI8658 stays uninitialized.
void    imu_hal_init(void) {}
void    imu_hal_tick(void) {}
uint8_t imu_hal_rotation_quadrant(void) { return 0; }

#endif
