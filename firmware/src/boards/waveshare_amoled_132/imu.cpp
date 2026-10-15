#include "../../hal/imu_hal.h"
#include "../../idle.h"
#include "board.h"
#include <Arduino.h>
#include <Preferences.h>

// No IMU on this board. The rotation quadrant is driven by the BOOT button
// instead: each press turns the UI 90° CW, persisted to NVS so it survives a
// reboot. display.cpp/touch.cpp read the quadrant exactly as on IMU boards.

#define BTN_POLL_MS 20

static uint8_t     rotation = 0;
static bool        last_down = false;
static uint32_t    last_poll_ms = 0;
static Preferences prefs;

void imu_hal_init(void) {
    pinMode(BTN_ROTATE_GPIO, INPUT_PULLUP);
    prefs.begin("rotation", false);
    rotation = prefs.getUChar("q", 0) & 3;
    Serial.printf("Rotation: %d (saved)\n", rotation);
}

void imu_hal_tick(void) {
    uint32_t now = millis();
    if (now - last_poll_ms < BTN_POLL_MS) return;
    last_poll_ms = now;

    bool down = digitalRead(BTN_ROTATE_GPIO) == LOW;
    // First press from sleep only wakes the panel, like the other buttons.
    if (down && !last_down && !idle_consume_wake_press()) {
        rotation = (rotation + 1) & 3;
        prefs.putUChar("q", rotation);
        Serial.printf("Rotation: %d\n", rotation);
    }
    last_down = down;
}

uint8_t imu_hal_rotation_quadrant(void) { return rotation; }
