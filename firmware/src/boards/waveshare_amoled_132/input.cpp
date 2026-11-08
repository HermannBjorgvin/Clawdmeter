#include "../../hal/input_hal.h"
#include "board.h"
#include <Arduino.h>

// BOOT is owned by the rotation handler in imu.cpp, so no button is ever
// reported to main.cpp's HID path (no Space / Shift+Tab on this board).

void input_hal_init(void) {}

bool input_hal_is_held(InputButton btn) {
    (void)btn;
    return false;
}
