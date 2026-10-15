#pragma once

// Optional board identity hooks. Shared code provides weak defaults
// (board_info_default.cpp); a board overrides only what it knows.

// Hardware revision of the board, as a short stable token for the `info`
// serial command and the desktop flasher ("" when the board has a single
// revision or can't tell). Valid after board_init(). Must be a static string.
//   waveshare_amoled_18: "sh8601_ft3168" | "co5300_cst816"
const char* board_hal_rev(void);
