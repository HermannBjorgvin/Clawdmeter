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
    .has_imu = false,
    .minimal_ui = true,        // e-paper: meters + a still corner Clawd, no animation
    .has_pwr_button = true,    // PWR side-button, GPIO18
    .has_touch = false,
};

const BoardCaps& board_caps(void) { return caps; }
