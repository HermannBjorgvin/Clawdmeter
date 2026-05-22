#include "../../hal/input_hal.h"
#include "board.h"
#include <Arduino.h>

// BOOT (GPIO0) → primary (Space PTT)
// VOL_DOWN (GPIO40) → secondary (Shift+Tab mode toggle)
// VOL_UP (GPIO39) → owned by power.cpp as the cycle-screens button

void input_hal_init(void) {
    pinMode(BTN_BACK_GPIO, INPUT_PULLUP);
    pinMode(BTN_FWD_GPIO, INPUT_PULLUP);
}

bool input_hal_is_held(InputButton btn) {
    switch (btn) {
    case INPUT_BTN_PRIMARY:
        return digitalRead(BTN_BACK_GPIO) == LOW;
    case INPUT_BTN_SECONDARY:
        return digitalRead(BTN_FWD_GPIO) == LOW;
    }
    return false;
}
