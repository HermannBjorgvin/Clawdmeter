#include "touch.h"
#include "ui.h"
#include "ble.h"
#include <Arduino.h>
#include "display_cfg.h"

// Thresholds
#define SWIPE_MIN_PX       50
#define SWIPE_MAX_CROSS_PX 30
#define HOLD_TIME_MS       500
#define LOGO_HOLD_TIME_MS  800
#define TAP_MAX_MOVE_PX    15
#define LOGO_X_MAX         100
#define LOGO_Y_MAX         100
#define MARGIN             20

enum touch_state_t {
    TS_IDLE,
    TS_DOWN,
    TS_HOLDING,
    TS_SWIPING,
    TS_LOGO_HOLD,
};

static touch_state_t state = TS_IDLE;
static uint16_t start_x, start_y;
static uint16_t last_x, last_y;
static uint32_t down_time;

// Shared touch state (read once per loop in main.cpp)
extern volatile bool     touch_pressed;
extern volatile uint16_t touch_x;
extern volatile uint16_t touch_y;

static bool get_touch(uint16_t* x, uint16_t* y) {
    if (touch_pressed) {
        *x = touch_x;
        *y = touch_y;
        return true;
    }
    return false;
}

static gesture_t classify_swipe(int dx, int dy) {
    int ax = abs(dx);
    int ay = abs(dy);
    if (ax > ay && ax >= SWIPE_MIN_PX && ay <= SWIPE_MAX_CROSS_PX)
        return dx > 0 ? GESTURE_SWIPE_RIGHT : GESTURE_SWIPE_LEFT;
    if (ay > ax && ay >= SWIPE_MIN_PX && ax <= SWIPE_MAX_CROSS_PX)
        return dy > 0 ? GESTURE_SWIPE_DOWN : GESTURE_SWIPE_UP;
    return GESTURE_NONE;
}

static bool in_logo(uint16_t x, uint16_t y) {
    return x < LOGO_X_MAX && y < LOGO_Y_MAX;
}

// Controller zone coordinates for 480x480 layout (scaled, MARGIN=20, CONTENT_Y=100)
// Side zones: 110x138, starting at y=100
#define ZONE_LEFT_X_MAX    130
#define ZONE_RIGHT_X_MIN   350
#define ZONE_TOP_Y_MIN     100
#define ZONE_TOP_Y_MAX     238
#define ZONE_BOT_Y_MIN     248
#define ZONE_BOT_Y_MAX     386

static gesture_t classify_zone_tap(uint16_t x, uint16_t y) {
    if (x < ZONE_LEFT_X_MAX) {
        if (y >= ZONE_TOP_Y_MIN && y < ZONE_TOP_Y_MAX) return GESTURE_TAP_ESCAPE;
        if (y >= ZONE_BOT_Y_MIN && y < ZONE_BOT_Y_MAX) return GESTURE_TAP_ARROW_LEFT;
    }
    if (x > ZONE_RIGHT_X_MIN) {
        if (y >= ZONE_TOP_Y_MIN && y < ZONE_TOP_Y_MAX) return GESTURE_TAP_BACKSPACE;
        if (y >= ZONE_BOT_Y_MIN && y < ZONE_BOT_Y_MAX) return GESTURE_TAP_ARROW_RIGHT;
    }
    return GESTURE_NONE;
}

static bool is_in_tap_zone(uint16_t x, uint16_t y) {
    return classify_zone_tap(x, y) != GESTURE_NONE;
}

// BLE clear zone on Bluetooth screen (scaled layout)
#define BLE_CLEAR_Y_MIN  276
#define BLE_CLEAR_Y_MAX  346

static bool in_ble_clear_zone(uint16_t x, uint16_t y) {
    return x >= MARGIN && x <= (480 - MARGIN) && y >= BLE_CLEAR_Y_MIN && y < BLE_CLEAR_Y_MAX;
}

void touch_init(void) {
    state = TS_IDLE;
}

void touch_tick(void) {
    uint16_t x, y;
    bool touching = get_touch(&x, &y);
    uint32_t now = millis();

    switch (state) {
    case TS_IDLE:
        if (touching) {
            start_x = x;
            start_y = y;
            last_x = x;
            last_y = y;
            down_time = now;
            state = TS_DOWN;
        }
        break;

    case TS_DOWN:
        if (!touching) {
            bool is_tap = abs((int)last_x - (int)start_x) < TAP_MAX_MOVE_PX &&
                          abs((int)last_y - (int)start_y) < TAP_MAX_MOVE_PX;

            if (is_tap && in_logo(start_x, start_y)) {
                ui_cycle_screen();
            } else if (is_tap && ui_get_current_screen() == SCREEN_CONTROLLER) {
                gesture_t zone = classify_zone_tap(start_x, start_y);
                if (zone != GESTURE_NONE) {
                    hid_on_gesture(zone);
                }
            } else if (is_tap && ui_get_current_screen() == SCREEN_BLUETOOTH) {
                if (in_ble_clear_zone(start_x, start_y)) {
                    ble_clear_bonds();
                }
            }
            state = TS_IDLE;
        } else {
            last_x = x;
            last_y = y;
            int dx = (int)x - (int)start_x;
            int dy = (int)y - (int)start_y;

            if (in_logo(start_x, start_y) && ui_get_current_screen() == SCREEN_CONTROLLER && (now - down_time) >= LOGO_HOLD_TIME_MS) {
                hid_on_gesture(GESTURE_TAP_CTRL_SPACE);
                state = TS_LOGO_HOLD;
            }
            else if (ui_get_current_screen() == SCREEN_CONTROLLER) {
                if (abs(dx) >= SWIPE_MIN_PX || abs(dy) >= SWIPE_MIN_PX) {
                    state = TS_SWIPING;
                } else if (!in_logo(start_x, start_y) && !is_in_tap_zone(start_x, start_y) && (now - down_time) >= HOLD_TIME_MS) {
                    hid_on_gesture(GESTURE_HOLD_START);
                    state = TS_HOLDING;
                }
            }
        }
        break;

    case TS_HOLDING:
        if (!touching) {
            hid_on_gesture(GESTURE_HOLD_END);
            state = TS_IDLE;
        }
        break;

    case TS_SWIPING:
        if (touching) {
            last_x = x;
            last_y = y;
        } else {
            int dx = (int)last_x - (int)start_x;
            int dy = (int)last_y - (int)start_y;
            gesture_t g = classify_swipe(dx, dy);
            if (g != GESTURE_NONE) {
                hid_on_gesture(g);
            }
            state = TS_IDLE;
        }
        break;

    case TS_LOGO_HOLD:
        if (!touching) {
            state = TS_IDLE;
        }
        break;
    }
}
