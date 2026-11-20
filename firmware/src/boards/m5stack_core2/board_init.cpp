#include "board.h"
#include "pmu.h"
#include <Arduino.h>
#include <Wire.h>

// Power rails and the panel reset live on the power chip, so they must be up
// before display_hal_init(). The microSD shares the panel's SPI bus: keep its
// chip select high.
extern "C" void board_init(void) {
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
    Wire.begin(IIC_SDA, IIC_SCL, 400000);
    pmu_init();
}

// Reported by the `info` serial command (hal/board_info_hal.h).
#include "../../hal/board_info_hal.h"
const char* board_hal_rev(void) {
    switch (pmu_kind()) {
    case PMU_AXP2101: return "v1.1_axp2101";
    case PMU_AXP192:  return "v1.0_axp192";
    default:          return "";
    }
}
