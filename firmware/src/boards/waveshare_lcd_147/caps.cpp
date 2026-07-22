#include "../../hal/board_caps.h"
#include "board.h"

static const BoardCaps caps = {
    .name = BOARD_NAME,
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    // Zero HID buttons: the kit's only usable key (BOOT) is given the PWR
    // role in power.cpp — see the comment in board.h. Shared code handles a
    // board with no primary button (the HID Space / Shift+Tab paths simply
    // never fire).
    .button_count = 0,
    .has_rotation = (bool)BOARD_HAS_ROTATION,
    .has_battery  = (bool)BOARD_HAS_BATTERY,
    .has_imu      = (bool)BOARD_HAS_IMU,
};

const BoardCaps& board_caps(void) { return caps; }
