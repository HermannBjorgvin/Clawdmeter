#pragma once

// Two shipping hardware revisions of the Waveshare 1.8" AMOLED differ only in
// their bonded panel module:
//   - original: SH8601 display (QSPI) + FT3168 touch (I2C 0x38)
//   - later:    CO5300 display (QSPI) + CST816 touch (I2C 0x15)
// Everything else (PMU, IMU, IO expander, pinout) is identical. We detect the
// revision at boot by probing the touch controller's I2C address — board_init()
// sets it BEFORE display_hal_init()/touch_hal_init() so both pick the right
// driver. The touch chip's presence is a reliable proxy for the panel module.

enum BoardRev {
    REV_SH8601_FT3168 = 0,   // original
    REV_CO5300_CST816 = 1,   // later revision
};

// Valid only after board_init() has run.
BoardRev board_rev(void);
