#include "../../hal/input_hal.h"
#include "board.h"
#include <Arduino.h>

// BOOT (GPIO0) is the only HID button here — the board's other readable button
// carries the PWR role (power.cpp) because without touch it is the only way to
// leave the splash screen. SECONDARY is never queried: caps.button_count is 1.

void input_hal_init(void) {
    pinMode(BTN_BACK_GPIO, INPUT_PULLUP);
}

bool input_hal_is_held(InputButton btn) {
    switch (btn) {
    case INPUT_BTN_PRIMARY:   return digitalRead(BTN_BACK_GPIO) == LOW;
    case INPUT_BTN_SECONDARY: return false;
    }
    return false;
}
