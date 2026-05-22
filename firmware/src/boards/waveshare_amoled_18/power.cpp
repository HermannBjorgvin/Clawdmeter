#include "../../hal/power_hal.h"
#include "board.h"
#include "io_expander.h"
#include <Arduino.h>
#include <Wire.h>
#include <XPowersLib.h>

// PWR button comes from XCA9554 EXIO4 (active HIGH). The PMU still
// provides battery monitoring; we just don't subscribe to its PKEY IRQ.

#define BATTERY_POLL_MS  2000
#define CHARGING_POLL_MS 500
#define PWR_POLL_MS      50

static XPowersPMU pmu;

static int      cached_pct       = -1;
static bool     cached_charging  = false;
static bool     cached_vbus      = false;
static bool     pwr_pressed_flag = false;
static bool     last_pwr_state   = false;   // edge detector for EXIO4
static uint32_t last_battery_ms  = 0;
static uint32_t last_charging_ms = 0;
static uint32_t last_pwr_ms      = 0;

void power_hal_init(void) {
    if (!pmu.begin(Wire, AXP2101_ADDR, IIC_SDA, IIC_SCL)) {
        Serial.println("AXP2101 init failed");
        return;
    }
    Serial.println("AXP2101 init OK");

    pmu.enableBattDetection();
    pmu.enableBattVoltageMeasure();
    // No PMU IRQ wiring — PWR comes via io_expander_get() below.

    cached_charging = pmu.isCharging();
    cached_vbus     = pmu.isVbusIn();
    cached_pct = pmu.getBatteryPercent();
}

void power_hal_tick(void) {
    uint32_t now = millis();

    if (now - last_charging_ms >= CHARGING_POLL_MS) {
        last_charging_ms = now;
        cached_charging = pmu.isCharging();
        cached_vbus     = pmu.isVbusIn();
    }
    if (now - last_battery_ms >= BATTERY_POLL_MS) {
        last_battery_ms = now;
        cached_pct = pmu.getBatteryPercent();
    }
    if (now - last_pwr_ms >= PWR_POLL_MS) {
        last_pwr_ms = now;
        bool pwr_now = io_expander_get(IOX_PIN_PWR_BTN);
        if (pwr_now && !last_pwr_state) {
            pwr_pressed_flag = true;
        }
        last_pwr_state = pwr_now;
    }
}

int  power_hal_battery_pct(void) { return cached_pct; }
bool power_hal_is_charging(void) { return cached_charging; }
bool power_hal_is_vbus_in(void)  { return cached_vbus; }

bool power_hal_pwr_pressed(void) {
    if (pwr_pressed_flag) {
        pwr_pressed_flag = false;
        return true;
    }
    return false;
}
