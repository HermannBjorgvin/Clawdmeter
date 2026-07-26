#include "board.h"
#include "io_expander.h"
#include <Arduino.h>
#include <Wire.h>

// Bring up the shared I2C bus, then run the TCA9554 power-on sequence that
// applies power to the display (P4) and touch (P5). This MUST happen before
// display_hal_init() / touch_hal_init() — both controllers are unpowered
// until the expander drives their rails HIGH.
//
// Unlike the C6 2.16, the LCD here is not fed from an AXP2101 ALDO rail, so
// no PMU rail config is needed at this stage; power.cpp brings the PMU up
// later for battery monitoring + PKEY handling. (If the panel ever comes up
// black despite the expander sequence, enabling the AXP ALDO rails here is
// the first thing to try.)

extern "C" void board_init(void) {
    Wire.begin(IIC_SDA, IIC_SCL);
    io_expander_init();
}
