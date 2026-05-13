#include "power.h"
#include "display_cfg.h"
#include <Arduino.h>

// Poll intervals
#define BATTERY_POLL_MS   2000
#define CHARGING_POLL_MS  500

static int      cached_pct      = -1;
static bool     cached_charging = false;
static bool     pwr_pressed_flag = false;
static bool     pwr_long_press_flag = false;
static uint32_t last_battery_ms  = 0;
static uint32_t last_charging_ms = 0;
static uint32_t last_pwr_ms      = 0;
#define PWR_POLL_MS 50

void power_init(void) {
    if (!pmu.begin(Wire, AXP2101_ADDR, IIC_SDA, IIC_SCL)) {
        Serial.println("AXP2101 init failed");
        return;
    }
    Serial.println("AXP2101 init OK");

    pmu.enableBattDetection();
    pmu.enableBattVoltageMeasure();

    // Enable PWR button short-press (cycle / wake) AND long-press (shutdown).
    pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    pmu.clearIrqStatus();
    pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ | XPOWERS_AXP2101_PKEY_LONG_IRQ);

    cached_charging = pmu.isCharging();
    cached_pct = pmu.getBatteryPercent();
}

void power_tick(void) {
    uint32_t now = millis();

    if (now - last_charging_ms >= CHARGING_POLL_MS) {
        last_charging_ms = now;
        cached_charging = pmu.isCharging();
    }

    if (now - last_battery_ms >= BATTERY_POLL_MS) {
        last_battery_ms = now;
        cached_pct = pmu.getBatteryPercent();
    }

    // Poll PWR button (AXP2101 short-press AND long-press IRQs)
    if (now - last_pwr_ms >= PWR_POLL_MS) {
        last_pwr_ms = now;
        pmu.getIrqStatus();
        if (pmu.isPekeyShortPressIrq()) pwr_pressed_flag      = true;
        if (pmu.isPekeyLongPressIrq())  pwr_long_press_flag   = true;
        pmu.clearIrqStatus();
    }
}

int power_battery_pct(void) {
    return cached_pct;
}

bool power_is_charging(void) {
    return cached_charging;
}

bool power_pwr_pressed(void) {
    if (pwr_pressed_flag) {
        pwr_pressed_flag = false;
        return true;
    }
    return false;
}

bool power_pwr_long_pressed(void) {
    if (pwr_long_press_flag) {
        pwr_long_press_flag = false;
        return true;
    }
    return false;
}

void power_shutdown(void) {
    Serial.println("AXP2101 shutdown requested");
    delay(50);  // flush the line
    pmu.shutdown();
}
