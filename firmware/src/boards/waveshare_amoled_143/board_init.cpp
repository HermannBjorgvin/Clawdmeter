#include "board.h"
#include "board_rev.h"
#include <Arduino.h>
#include <Wire.h>

// No IO expander on this board — the display/touch reset lines are direct
// GPIO, so all we do at boot is start the I2C bus and sniff the panel
// revision from the touch controller address.

static BoardRev g_rev = REV_CO5300_CST816;   // default to current production rev

BoardRev board_rev(void) { return g_rev; }

static bool i2c_present(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

extern "C" void board_init(void) {
    Wire.begin(IIC_SDA, IIC_SCL);
    delay(10);

    // CST820 (current) answers at 0x15; FT3168 (early) at 0x38.
    if (i2c_present(CST816_ADDR)) {
        g_rev = REV_CO5300_CST816;
        Serial.println("Board revision: CO5300 + CST820 (0x15)");
    } else if (i2c_present(FT3168_ADDR)) {
        g_rev = REV_SH8601_FT3168;
        Serial.println("Board revision: SH8601 + FT3168 (0x38)");
    } else {
        Serial.println("WARNING: no touch controller found on I2C "
                       "(47/48) — defaulting to CO5300 + CST820");
    }
}
