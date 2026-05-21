#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// The panel and touch controller are powered by an LDO rail gated by
// PMICEN. The display init touches I2C-PMU registers and the QSPI lines,
// both of which sit behind that rail — so PMICEN must go HIGH before
// display_hal_init() runs. The 50 ms settling time matches what worked
// reliably during the original bring-up; shorter delays produced
// intermittent panel-init failures from boot.
extern "C" void board_init(void) {
    pinMode(PMICEN_GPIO, OUTPUT);
    digitalWrite(PMICEN_GPIO, HIGH);
    delay(50);

    Wire.begin(IIC_SDA, IIC_SCL);
}
