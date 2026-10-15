#include <stdlib.h>

#include "../../hal/board_caps.h"
#include "board.h"

// SIM_NO_PWR=1 plays a board without a PWR button (LCD-4): the p key does
// nothing and the pairing hint offers the touch hold instead. SIM_MINIMAL=1
// plays a minimal-mode board. SIM_NO_TOUCH=1 plays a board without a
// touchscreen (BOOT dismisses an alert). SIM_CIRCLE=1 plays a round panel (the
// corners outside the circle are drawn as a grey bezel). SIM_BUTTONS=0|1|2 plays
// a board with no buttons / BOOT only / BOOT + secondary.
static BoardCaps make_caps(void) {
    BoardCaps caps = {
    .name = BOARD_NAME,
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .round_panel = false,    // SIM_CIRCLE=1 below
    .button_count = 2,      // B and N keys stand in for BOOT + GPIO18
    .has_rotation = false,
    .has_battery = true,    // fake battery, adjustable with -/=
    .has_imu = false,
    .minimal_ui   = false,   // full animated UI; true for e-ink / slow panels
    .has_pwr_button = true,  // the p key
    .has_touch = true,       // the mouse
    };
    const char* no_pwr = getenv("SIM_NO_PWR");
    if (no_pwr && *no_pwr && *no_pwr != '0') caps.has_pwr_button = false;
    const char* no_touch = getenv("SIM_NO_TOUCH");
    if (no_touch && *no_touch && *no_touch != '0') caps.has_touch = false;
    const char* minimal = getenv("SIM_MINIMAL");
    if (minimal && *minimal && *minimal != '0') caps.minimal_ui = true;
    const char* buttons = getenv("SIM_BUTTONS");
    if (buttons && (*buttons >= '0' && *buttons <= '2')) caps.button_count = (uint8_t)(*buttons - '0');
    const char* circle = getenv("SIM_CIRCLE");
    if (circle && *circle && *circle != '0') caps.round_panel = true;
    return caps;
}

const BoardCaps& board_caps(void) {
    static const BoardCaps caps = make_caps();
    return caps;
}
