#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <XPowersLib.h>

// On this C6 board the SH8601 LCD + CST9217 touch are powered from the
// AXP2101's ALDO rails, not directly from 3V3. The display init must run
// AFTER the rails are up, so we bring the PMU up here in board_init()
// (before display_hal_init) instead of in power_hal_init() which runs
// later. power.cpp re-uses the same XPowersPMU handle for battery polling.
//
// Rail config mirrors the Waveshare XiaoZhi BSP for this board:
//   DC1 = 3.3V (system rail)
//   ALDO1..4 = 3.3V, all enabled (LCD, touch, sensors)

XPowersPMU board_pmu;   // shared instance — power.cpp uses extern reference

extern "C" void board_init(void) {
    Wire.begin(IIC_SDA, IIC_SCL);

    if (!board_pmu.begin(Wire, AXP2101_ADDR, IIC_SDA, IIC_SCL)) {
        Serial.println("AXP2101 init failed (board_init)");
        return;
    }

    // Set all four ALDOs to 3.3V.
    board_pmu.setALDO1Voltage(3300);
    board_pmu.setALDO2Voltage(3300);
    board_pmu.setALDO3Voltage(3300);
    board_pmu.setALDO4Voltage(3300);

    // ALDO1, 2, 4 just need to be enabled.
    board_pmu.enableALDO1();
    board_pmu.enableALDO2();
    board_pmu.enableALDO4();

    // ALDO3 doubles as the LCD reset line on this board (the SH8601's RST
    // pin isn't wired to a MCU GPIO). The Waveshare BSP pulses it
    // HIGH → LOW → HIGH with 100 ms holds to issue a proper reset before
    // the panel sees its first SPI command. Without this pulse the SH8601
    // stays in an indeterminate state and the screen is black even though
    // QSPI init and brightness writes succeed.
    board_pmu.enableALDO3();
    delay(100);
    board_pmu.disableALDO3();
    delay(100);
    board_pmu.enableALDO3();
    delay(100);

    Serial.println("AXP2101 rails up (ALDO1-4 @ 3.3V, LCD reset pulsed)");
}
