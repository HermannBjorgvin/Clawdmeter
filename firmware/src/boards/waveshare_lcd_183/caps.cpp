#include "../../hal/board_caps.h"
#include "board.h"

static const BoardCaps caps = {
    .name = BOARD_NAME,
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .round_panel = false,
    .button_count = 1,
    .has_rotation = false,
    .has_battery = true,
    .has_imu = true,
    .minimal_ui = false,     // full animated UI
    .has_pwr_button = true,  // AXP2101 PKEY, see power.cpp
    .has_touch = true,
};

const BoardCaps& board_caps(void) { return caps; }
