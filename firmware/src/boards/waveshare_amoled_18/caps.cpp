#include "../../hal/board_caps.h"
#include "board.h"

static const BoardCaps caps = {
    .name = BOARD_NAME,
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .button_count = 1,
    .has_rotation = false,
    .has_battery = true,
    .has_imu = true,
    .has_touch = true,        // FT3168 cap touch
};

const BoardCaps& board_caps(void) { return caps; }
