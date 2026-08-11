#include "../../hal/board_caps.h"
#include "board.h"

static const BoardCaps caps = {
    .name = BOARD_NAME,
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .button_count = 2,      // B and N keys stand in for BOOT + GPIO18
    .has_rotation = false,
    .has_battery = true,    // fake battery, adjustable with -/=
    .has_imu = false,
};

const BoardCaps& board_caps(void) { return caps; }
