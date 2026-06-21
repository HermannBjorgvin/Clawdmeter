#include "../../hal/board_caps.h"
#include "board.h"

static const BoardCaps caps = {
    .name = BOARD_NAME,
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .button_count = 1,
    .has_rotation = false,
    .has_battery = false,   // "Without BAT" SKU — PMU populated, no cell. Set true if you add one.
    .has_imu = true,
};

const BoardCaps& board_caps(void) { return caps; }
