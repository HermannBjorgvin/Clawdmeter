#include "buttons.h"
#include "display_cfg.h"
#include "ble.h"
#include "ui.h"
#include "splash.h"
#include "power.h"
#include <Arduino.h>

// HID usage codes (USB HID 1.11, Keyboard/Keypad page).
#define HID_KEY_TAB    0x2B
#define HID_KEY_SPACE  0x2C
#define HID_MOD_SHIFT  0x02

static inline bool btn_is_pressed(int gpio) {
#if BOARD_BTN_ACTIVE_LOW
    return digitalRead(gpio) == LOW;
#else
    return digitalRead(gpio) == HIGH;
#endif
}

#if defined(BOARD_WAVESHARE_AMOLED_216)

// Waveshare keeps the original 3-button scheme:
//   - BOARD_BTN0 (GPIO 0)  edge -> Space press / release (voice mode)
//   - BOARD_BTN1 (GPIO 18) edge -> Shift+Tab press / release (mode toggle)
//   - PMU PKEY  short      -> cycle screens; on splash, cycle animations
// The Space/Shift-Tab keys mirror the button state directly so users can
// hold them — matches how Claude Code's voice mode wants push-to-talk.

void buttons_init(void) {
    pinMode(BOARD_BTN0_GPIO, INPUT_PULLUP);
    pinMode(BOARD_BTN1_GPIO, INPUT_PULLUP);
}

void buttons_tick(void) {
    static bool b0_was = false, b1_was = false;
    bool b0_now = btn_is_pressed(BOARD_BTN0_GPIO);
    bool b1_now = btn_is_pressed(BOARD_BTN1_GPIO);

    if (b0_now != b0_was) {
        if (b0_now) ble_keyboard_press(HID_KEY_SPACE, 0);
        else        ble_keyboard_release();
        b0_was = b0_now;
    }
    if (b1_now != b1_was) {
        if (b1_now) ble_keyboard_press(HID_KEY_TAB, HID_MOD_SHIFT);
        else        ble_keyboard_release();
        b1_was = b1_now;
    }

#if BOARD_HAS_PWR_BUTTON
    if (power_pwr_pressed()) {
        if (ui_get_current_screen() == SCREEN_SPLASH) splash_next();
        else                                          ui_cycle_screen();
    }
#endif
}

#elif defined(BOARD_LILYGO_T_DISPLAY_S3)

// LILYGO only has two physical buttons, so each one carries a short and a
// long action:
//   BTN0 (BOOT / GPIO 0)
//     - short press      -> toggle to/from the splash screen
//     - long press hold  -> Space held (voice push-to-talk)
//   BTN1 (IO14)
//     - short press      -> cycle screens (or, on splash, next animation)
//     - long press edge  -> Shift+Tab once
//
// BOOT is also a strap pin — holding it during reset puts the chip into
// download mode. After boot it behaves like a normal input.

#define BTN_DEBOUNCE_MS    25
#define BTN_LONG_PRESS_MS  600

struct ButtonState {
    int      gpio;
    bool     stable;          // debounced pressed state
    bool     raw_last;        // last raw sample
    uint32_t raw_since;       // when raw_last started
    uint32_t press_started;   // when stable went from up -> down
    bool     long_fired;      // long-press action already taken this hold
    bool     space_active;    // true while we're holding Space for this button
};

static ButtonState btn0 = { BOARD_BTN0_GPIO, false, false, 0, 0, false, false };
static ButtonState btn1 = { BOARD_BTN1_GPIO, false, false, 0, 0, false, false };

static void btn_update(ButtonState* b) {
    uint32_t now = millis();
    bool raw = btn_is_pressed(b->gpio);
    if (raw != b->raw_last) {
        b->raw_last = raw;
        b->raw_since = now;
        return;
    }
    if (now - b->raw_since < BTN_DEBOUNCE_MS) return;
    if (raw == b->stable) return;

    b->stable = raw;
    if (raw) {
        b->press_started = now;
        b->long_fired = false;
    }
}

static bool btn_held_long(const ButtonState* b) {
    return b->stable && !b->long_fired &&
           (millis() - b->press_started >= BTN_LONG_PRESS_MS);
}

void buttons_init(void) {
    pinMode(BOARD_BTN0_GPIO, INPUT_PULLUP);
    pinMode(BOARD_BTN1_GPIO, INPUT_PULLUP);
}

void buttons_tick(void) {
    bool b0_prev = btn0.stable;
    bool b1_prev = btn1.stable;
    btn_update(&btn0);
    btn_update(&btn1);

    // BTN0: long-press begins -> hold Space; release -> release Space; a
    // short press (release before threshold and no Space sent) toggles
    // the splash screen.
    if (btn0.stable && !btn0.space_active && btn_held_long(&btn0)) {
        ble_keyboard_press(HID_KEY_SPACE, 0);
        btn0.space_active = true;
        btn0.long_fired = true;
    }
    if (!btn0.stable && b0_prev) {
        if (btn0.space_active) {
            ble_keyboard_release();
            btn0.space_active = false;
        } else {
            ui_toggle_splash();
        }
    }

    // BTN1: long-press fires Shift+Tab once and releases; short press
    // cycles screens (or next animation on splash).
    if (btn_held_long(&btn1)) {
        ble_keyboard_press(HID_KEY_TAB, HID_MOD_SHIFT);
        ble_keyboard_release();
        btn1.long_fired = true;
    }
    if (!btn1.stable && b1_prev) {
        if (!btn1.long_fired) {
            if (ui_get_current_screen() == SCREEN_SPLASH) splash_next();
            else                                          ui_cycle_screen();
        }
    }
}

#else
#  error "buttons.cpp: no board-specific implementation for the selected board"
#endif
