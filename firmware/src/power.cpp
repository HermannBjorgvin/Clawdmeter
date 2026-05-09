#include "power.h"
#include "display_cfg.h"
#include <Arduino.h>

// Poll intervals
#define BATTERY_POLL_MS   2000
#define VBUS_POLL_MS      500

static int      cached_pct      = -1;
static bool     cached_charging = false;
static bool     cached_usb      = false;
static bool     usb_changed_flag = false;
static bool     pwr_pressed_flag = false;
static uint32_t last_battery_ms  = 0;
static uint32_t last_vbus_ms     = 0;
static uint32_t last_pwr_ms      = 0;
#define PWR_POLL_MS 50

void power_init(void) {
    if (!pmu.begin(Wire, AXP2101_ADDR, IIC_SDA, IIC_SCL)) {
        Serial.println("AXP2101 init failed");
        return;
    }
    Serial.println("AXP2101 init OK");

    // Enable battery and VBUS ADC measurements
    pmu.enableBattDetection();
    pmu.enableVbusVoltageMeasure();
    pmu.enableBattVoltageMeasure();

    // Enable PWR button short-press IRQ (mid button for cycling screens)
    pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    pmu.clearIrqStatus();
    pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);

    // Read initial state
    cached_usb = pmu.isVbusIn();
    cached_charging = pmu.isCharging();
    cached_pct = pmu.getBatteryPercent();
}

void power_tick(void) {
    uint32_t now = millis();

    // Poll VBUS status
    if (now - last_vbus_ms >= VBUS_POLL_MS) {
        last_vbus_ms = now;
        bool usb_now = pmu.isVbusIn();
        if (usb_now != cached_usb) {
            cached_usb = usb_now;
            usb_changed_flag = true;
        }
        cached_charging = pmu.isCharging();
    }

    // Poll battery level
    if (now - last_battery_ms >= BATTERY_POLL_MS) {
        last_battery_ms = now;
        cached_pct = pmu.getBatteryPercent();
    }

    // Poll PWR button (AXP2101 short-press IRQ)
    if (now - last_pwr_ms >= PWR_POLL_MS) {
        last_pwr_ms = now;
        pmu.getIrqStatus();
        if (pmu.isPekeyShortPressIrq()) {
            pwr_pressed_flag = true;
        }
        pmu.clearIrqStatus();
    }
}

int power_battery_pct(void) {
    return cached_pct;
}

bool power_is_charging(void) {
    return cached_charging;
}

bool power_is_usb_connected(void) {
    return cached_usb;
}

bool power_usb_changed(void) {
    if (usb_changed_flag) {
        usb_changed_flag = false;
        return true;
    }
    return false;
}

bool power_pwr_pressed(void) {
    if (pwr_pressed_flag) {
        pwr_pressed_flag = false;
        return true;
    }
    return false;
}
