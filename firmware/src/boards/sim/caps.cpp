#include <stdlib.h>

#include "../../hal/board_caps.h"
#include "board.h"

// SIM_NO_PWR=1 plays a board without a PWR button (LCD-4): the p key does
// nothing and the pairing hint offers the touch hold instead. SIM_MINIMAL=1
// plays a minimal-mode board.
static BoardCaps make_caps(void) {
    BoardCaps caps = {
    .name = BOARD_NAME,
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .button_count = 2,      // B and N keys stand in for BOOT + GPIO18
    .has_rotation = false,
    .has_battery = true,    // fake battery, adjustable with -/=
    .has_imu = false,
    .minimal_ui   = false,   // full animated UI; true for e-ink / slow panels
    .has_pwr_button = true,  // the p key
    };
    const char* no_pwr = getenv("SIM_NO_PWR");
    if (no_pwr && *no_pwr && *no_pwr != '0') caps.has_pwr_button = false;
    const char* minimal = getenv("SIM_MINIMAL");
    if (minimal && *minimal && *minimal != '0') caps.minimal_ui = true;
    return caps;
}

const BoardCaps& board_caps(void) {
    static const BoardCaps caps = make_caps();
    return caps;
}
