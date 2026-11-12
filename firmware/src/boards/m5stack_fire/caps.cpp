#include "../../hal/board_caps.h"
#include "board.h"

static const BoardCaps caps = {
    .name = BOARD_NAME,
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .round_panel = false,
    // Front buttons A (primary) + C (secondary). The middle button B is the
    // PWR-role button, handled in power.cpp and not counted here.
    .button_count = (uint8_t)(1 + BOARD_HAS_SECONDARY_BUTTON),
    .has_rotation = (bool)BOARD_HAS_ROTATION,
    .has_battery  = (bool)BOARD_HAS_BATTERY,
    .has_imu      = (bool)BOARD_HAS_IMU,
    .minimal_ui   = false,   // full animated UI
    .has_pwr_button = true,  // middle button B (GPIO 38), PWR role: see power.cpp
    .has_touch = false,
};

const BoardCaps& board_caps(void) { return caps; }
