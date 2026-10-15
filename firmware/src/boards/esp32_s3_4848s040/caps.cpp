#include "../../hal/board_caps.h"
#include "board.h"

static const BoardCaps caps = {
    .name         = BOARD_NAME,
    .width        = LCD_WIDTH,
    .height       = LCD_HEIGHT,
    .round_panel = false,
    .button_count = 0,
    .has_rotation = false,
    .has_battery  = false,
    .has_imu      = false,
    .minimal_ui   = false,   // full animated UI; true for e-ink / slow panels
    .has_pwr_button = false, // no PWR or user button: pair from the computer / touch hold
    .has_touch = true,
};

const BoardCaps& board_caps(void) { return caps; }
