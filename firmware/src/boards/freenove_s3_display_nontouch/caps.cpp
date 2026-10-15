#include "../../hal/board_caps.h"
#include "board.h"

static const BoardCaps caps = {
    .name = BOARD_NAME,
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .round_panel = false,
    .button_count = 0,       // BOOT is the PWR role (power.cpp): no primary or secondary
    .has_rotation = (bool)BOARD_HAS_ROTATION,
    .has_battery  = (bool)BOARD_HAS_BATTERY,
    .has_imu      = (bool)BOARD_HAS_IMU,
    // Meters and a still corner Clawd only: the SPI TFT is slow.
    .minimal_ui   = true,
    .has_pwr_button = true,   // BOOT (GPIO0): brightness, hold-to-pair (power.cpp)
    .has_touch = false,
};

const BoardCaps& board_caps(void) { return caps; }
