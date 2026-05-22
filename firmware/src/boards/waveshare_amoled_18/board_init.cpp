#include "board.h"
#include "io_expander.h"
#include <Arduino.h>
#include <Wire.h>

// AMOLED-1.8 also needs the XCA9554 IO expander up first — the display
// and touch controllers stay in reset until EXIO0..1 go HIGH.
extern "C" void board_init(void) {
    Wire.begin(IIC_SDA, IIC_SCL);
    io_expander_init();
}
