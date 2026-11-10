#include "../../hal/board_caps.h"
#include "board.h"

static const BoardCaps caps = {
    .name         = BOARD_NAME,
    .width        = LCD_WIDTH,
    .height       = LCD_HEIGHT,
    .round_panel = false,
    .button_count = 0,   // no BOOT/primary key: the GPIO 38 key is the PWR role
    .has_rotation = (bool)BOARD_HAS_ROTATION,
    .has_battery  = (bool)BOARD_HAS_BATTERY,
    .has_imu      = (bool)BOARD_HAS_IMU,
    .minimal_ui   = false,   // full animated UI
    .has_pwr_button = true,  // GPIO38 user button, PWR role (power.cpp)
    .has_touch = true,
};

const BoardCaps& board_caps(void) { return caps; }
