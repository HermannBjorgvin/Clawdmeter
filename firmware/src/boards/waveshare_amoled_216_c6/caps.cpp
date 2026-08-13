#include "../../hal/board_caps.h"
#include "board.h"

static const BoardCaps caps = {
    .name = BOARD_NAME,
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    // BOOT (primary) + KEY (secondary) GPIO buttons. PWR is on the AXP
    // PKEY and handled separately (not counted here).
    .button_count = (uint8_t)(1 + BOARD_HAS_SECONDARY_BUTTON),
    .has_rotation = (bool)BOARD_HAS_ROTATION,
    .has_battery = (bool)BOARD_HAS_BATTERY,
    .has_imu = (bool)BOARD_HAS_IMU,
    .has_touch = true,
};

const BoardCaps& board_caps(void) { return caps; }
