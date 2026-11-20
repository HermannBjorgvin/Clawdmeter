#include "../../hal/input_hal.h"
#include "board.h"
#include "ft6336.h"

// Touch buttons A (left) and C (right) in the strip below the panel. B, the
// middle one, is the PWR role (power.cpp).

void input_hal_init(void) {}

bool input_hal_is_held(InputButton btn) {
    ft6336_poll();
    switch (btn) {
    case INPUT_BTN_PRIMARY:   return ft6336_zone() == TZ_BTN_A;
    case INPUT_BTN_SECONDARY: return ft6336_zone() == TZ_BTN_C;
    }
    return false;
}
