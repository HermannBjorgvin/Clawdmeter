#include "../../hal/input_hal.h"
#include "board.h"

// This kit has exactly one readable key (BOOT / GPIO 0) and power.cpp owns it
// as the PWR button — without it there is no way to open the BLE pairing
// window, which would make the device unusable. So there is no HID
// push-to-talk or mode-toggle button here: both report "not held" forever and
// BoardCaps.button_count is 0.

void input_hal_init(void) {}

bool input_hal_is_held(InputButton btn) {
    (void)btn;
    return false;
}
