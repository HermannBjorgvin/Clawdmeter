#include "board.h"
#include "pmu.h"
#include <Arduino.h>
#include <Wire.h>
#include <stdio.h>

// Power rails and the panel reset live on the power chip, so they must be up
// before display_hal_init(). The microSD shares the panel's SPI bus: keep its
// chip select high.
extern "C" void board_init(void) {
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
    Wire.begin(IIC_SDA, IIC_SCL, 400000);
    pmu_init();
}

// Reported by the `info` serial command (hal/board_info_hal.h): power chip and
// panel controller, e.g. "v1.1_axp2101_ili9342c". Valid after display_hal_init().
#include "../../hal/board_info_hal.h"
const char* core2_panel_name(void);  // display.cpp
const char* board_hal_rev(void) {
    static char rev[32];
    const char* pmu = "";
    switch (pmu_kind()) {
    case PMU_AXP2101: pmu = "v1.1_axp2101"; break;
    case PMU_AXP192:  pmu = "v1.0_axp192";  break;
    default:          break;
    }
    snprintf(rev, sizeof(rev), "%s_%s", pmu, core2_panel_name());
    return rev;
}
