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
    .minimal_ui   = false,   // full animated UI; true for e-ink / slow panels
    .has_pwr_button = true,  // XCA9554 EXIO4
};

const BoardCaps& board_caps(void) { return caps; }
