#include "../../hal/board_caps.h"
#include "board.h"

static const BoardCaps caps = {
    .name = BOARD_NAME,
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .round_panel = false,
    .button_count = 0,
    .has_rotation = false,
    .has_battery = false,
    .has_imu = false,
    .minimal_ui = false,
    .has_pwr_button = true,
    .has_touch = false,
    .pwr_button_is_boot = true,
};
const BoardCaps& board_caps(void) { return caps; }
