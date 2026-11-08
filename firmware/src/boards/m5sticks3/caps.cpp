#include "../../hal/board_caps.h"
#include "board.h"

static const BoardCaps caps = {
    .name = BOARD_NAME,
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .round_panel = false,
    // BtnA (front) is the only shared-code button; BtnB plays the PWR role
    // in power.cpp and is not counted here.
    .button_count = (uint8_t)(1 + BOARD_HAS_SECONDARY_BUTTON),
    .has_rotation = (bool)BOARD_HAS_ROTATION,
    .has_battery  = (bool)BOARD_HAS_BATTERY,
    .has_imu      = (bool)BOARD_HAS_IMU,
    .minimal_ui   = false,   // full animated UI; true for e-ink / slow panels
    .has_pwr_button = true,  // BtnB (GPIO12), PWR role
    .has_touch = false,
};

const BoardCaps& board_caps(void) { return caps; }
