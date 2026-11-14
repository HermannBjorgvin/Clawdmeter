#include "../../hal/board_caps.h"
#include "board.h"

static const BoardCaps caps = {
    .name = BOARD_NAME,
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .round_panel = true,
    .button_count = 1,
    .has_rotation = false,
    .has_battery = true,
    .has_imu = false,
    .minimal_ui = false,
    .has_pwr_button = true,
    .has_touch = true,
};

const BoardCaps& board_caps(void) { return caps; }
