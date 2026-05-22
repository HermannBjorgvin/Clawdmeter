#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// Called once at the very start of setup(), before any HAL device init.
// At minimum bring up the shared I2C bus. If your board has an IO expander
// gating the LCD or touch reset lines, initialize and release it here too
// (otherwise display_hal_init() will fail to probe the panel).
extern "C" void board_init(void) {
    Wire.begin(IIC_SDA, IIC_SCL);
    // TODO: io_expander_init() if your board needs one
}
