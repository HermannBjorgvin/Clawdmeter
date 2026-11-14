#include "../../hal/board_caps.h"
#include "board.h"
#include "io_expander.h"

static BoardCaps caps = {
    .name = BOARD_NAME,
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .button_count = (uint8_t)(1 + BOARD_HAS_SECONDARY_BUTTON),
    .has_rotation = (bool)BOARD_HAS_ROTATION,
    .has_battery  = false,   // set below once the expander is known
    .has_imu      = (bool)BOARD_HAS_IMU,
    .minimal_ui   = false,   // full animated UI; true for e-ink / slow panels
    .has_pwr_button = false,  // KEY/PWR is a hardware reset
    .has_touch = true,
};

// Battery telemetry exists only on the CH32V003 revision (its ADC); the older
// TCA9554 boards have none, so they show no battery indicator. board_init()
// probes the expander before anything reads the battery capability.
const BoardCaps& board_caps(void) {
    caps.has_battery = BOARD_HAS_BATTERY && io_expander_has_battery();
    return caps;
}
