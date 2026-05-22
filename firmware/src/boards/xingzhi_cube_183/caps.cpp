#include "../../hal/board_caps.h"
#include "board.h"

static const BoardCaps caps = {
    .name = BOARD_NAME,
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .button_count = 2,
    .has_rotation = false,
    .has_battery = true,
    .has_imu = false,
    .has_touch = false,       // no touchscreen — PWR drives splash exit
    .auto_cycle_ms = 5000,    // touchless → auto-cycle Usage <-> Bluetooth every 5 s
};

const BoardCaps& board_caps(void) { return caps; }
