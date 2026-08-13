#include "../../hal/board_caps.h"
#include "board.h"

static const BoardCaps caps = {
    .name = BOARD_NAME,
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    // BOOT only. The board's other readable button (GPIO35) carries the PWR
    // role and is handled in power.cpp, so it isn't counted here; the third
    // button is RST, wired to EN and invisible to software.
    .button_count = (uint8_t)(1 + BOARD_HAS_SECONDARY_BUTTON),
    .has_rotation = (bool)BOARD_HAS_ROTATION,
    .has_battery  = (bool)BOARD_HAS_BATTERY,
    .has_imu      = (bool)BOARD_HAS_IMU,
    .has_touch    = (bool)BOARD_HAS_TOUCH,
};

const BoardCaps& board_caps(void) { return caps; }
