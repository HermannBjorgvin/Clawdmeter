#include "../../hal/input_hal.h"
#include "board.h"
#include <Arduino.h>

// Only the BOOT button is wired as a user input — GPIO 18 carries the
// panel TE signal so the second-button slot stays empty. Screen cycling
// piggybacks on a long press of the same GPIO, synthesized as a PWR
// press inside power.cpp.

void input_hal_init(void) {
    pinMode(BTN_BACK_GPIO, INPUT_PULLUP);
}

bool input_hal_is_held(InputButton btn) {
    switch (btn) {
    case INPUT_BTN_PRIMARY:
        return digitalRead(BTN_BACK_GPIO) == LOW;
    case INPUT_BTN_SECONDARY:
        return false;
    }
    return false;
}
