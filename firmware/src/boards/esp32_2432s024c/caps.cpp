#include "../../hal/board_caps.h"
#include "board.h"

static const BoardCaps caps = {
    .name = BOARD_NAME,
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .round_panel = false,
    // The one user button (BOOT, GPIO 0) takes the PWR role in power.cpp, so
    // there is no HID button.
    .button_count = 0,
    .has_rotation = false,
    .has_battery = false,
    .has_imu = false,
    .minimal_ui = false,
    .has_pwr_button = true,  // BOOT (GPIO 0), see power.cpp
    .has_touch = true,
};

const BoardCaps& board_caps(void) { return caps; }
