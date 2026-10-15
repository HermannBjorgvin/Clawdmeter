#pragma once

// Two shipping panel revisions, distinguished by which touch controller
// answers on I2C. board_init() sets this; display.cpp and touch.cpp read it.
enum BoardRev {
    REV_SH8601_FT3168 = 0,   // early units
    REV_CO5300_CST816 = 1,   // current units (CO5300 + CST820 @ 0x15)
};

BoardRev board_rev(void);
