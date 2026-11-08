#include "../../hal/input_hal.h"
#include "board.h"
#include <Arduino.h>

// M5Stack front buttons A (left) and C (right) are the two screen-independent
// buttons. The middle button B is the PWR-role button and lives in power.cpp.
// GPIO 37/38/39 are input-only with no internal pull-up; M5Stack provides
// external 10k pull-ups, so these use plain INPUT and read LOW when pressed.

void input_hal_init(void) {
    pinMode(BTN_A_GPIO, INPUT);
#if BOARD_HAS_SECONDARY_BUTTON
    pinMode(BTN_C_GPIO, INPUT);
#endif
}

bool input_hal_is_held(InputButton btn) {
    switch (btn) {
    case INPUT_BTN_PRIMARY:
        return digitalRead(BTN_A_GPIO) == LOW;
    case INPUT_BTN_SECONDARY:
#if BOARD_HAS_SECONDARY_BUTTON
        return digitalRead(BTN_C_GPIO) == LOW;
#else
        return false;
#endif
    }
    return false;
}
