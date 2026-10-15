#include "../../hal/board_caps.h"
#include "board.h"

static const BoardCaps caps = {
    .name = BOARD_NAME,
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .round_panel = true,
    .button_count = 1,   // BOOT is consumed by rotation; input_hal reports it never held
    .has_rotation = (bool)BOARD_HAS_ROTATION,
    .has_battery  = (bool)BOARD_HAS_BATTERY,
    .has_imu      = (bool)BOARD_HAS_IMU,
    .minimal_ui   = false,
    .has_pwr_button = true,  // GPIO17, PWR role
    .has_touch = true,
};

const BoardCaps& board_caps(void) { return caps; }
