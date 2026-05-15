#include "imu.h"
#include "display_cfg.h"
#include <Arduino.h>

#if BOARD_HAS_IMU

// Auto-rotation from the QMI8658 accelerometer's gravity vector. Active
// only on boards that physically have an IMU; everywhere else the
// rotation is fixed at BOARD_FIXED_ROTATION and we don't even compile
// the polling code.

#define IMU_POLL_MS       100    // ~10 Hz accel polling
#define STABLE_TIME_MS    300    // hold the new orientation for this long
#define TILT_THRESHOLD    0.5f   // ~30 deg from axis (sin(30) ≈ 0.5)

static uint8_t  current_rotation = BOARD_FIXED_ROTATION;
static uint8_t  candidate_rotation = BOARD_FIXED_ROTATION;
static uint32_t candidate_since = 0;
static uint32_t last_poll_ms = 0;
static bool     imu_ok = false;

static uint8_t accel_to_rotation(float ax, float ay) {
    float abs_ax = fabsf(ax);
    float abs_ay = fabsf(ay);

    // Ambiguous when laid flat — keep the current orientation.
    if (abs_ax < TILT_THRESHOLD && abs_ay < TILT_THRESHOLD) return 255;

    if (abs_ay > abs_ax) return (ay > 0) ? 3 : 1;
    else                 return (ax > 0) ? 0 : 2;
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

uint8_t imu_get_rotation(void) { return current_rotation; }

#else

// No IMU on this board — rotation is a compile-time constant pulled from
// the board header, and the tick is a no-op.
void    imu_init(void)         { Serial.println("imu: not present on this board"); }
void    imu_tick(void)         {}
uint8_t imu_get_rotation(void) { return BOARD_FIXED_ROTATION; }

#endif
