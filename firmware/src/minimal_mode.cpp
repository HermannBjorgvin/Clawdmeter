#include "minimal_mode.h"
#include "hal/board_caps.h"

bool minimal_mode(void) { return board_caps().minimal_ui; }
