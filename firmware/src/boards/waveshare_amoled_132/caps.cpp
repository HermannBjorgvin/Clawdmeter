#include "../../hal/board_caps.h"
#include "board.h"

static const BoardCaps caps = {
    .name = BOARD_NAME,
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .round_panel = true,
    .button_count = 0,   // BOOT rotates the UI (imu.cpp); input_hal never reports it held
    .has_rotation = (bool)BOARD_HAS_ROTATION,
    .has_battery  = (bool)BOARD_HAS_BATTERY,
    .has_imu      = (bool)BOARD_HAS_IMU,
    .minimal_ui   = false,
    .has_pwr_button = true,  // GPIO17, PWR role
    .has_touch = true,
};

const BoardCaps& board_caps(void) { return caps; }
