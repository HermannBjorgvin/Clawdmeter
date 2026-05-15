#include "power.h"
#include "display_cfg.h"
#include <Arduino.h>

#ifndef BAT_CHARGE_DETECT_USB_CDC
#define BAT_CHARGE_DETECT_USB_CDC 0
#endif

#if BOARD_HAS_BATTERY_ADC && BAT_CHARGE_DETECT_USB_CDC
#include <HWCDC.h>
#endif

// Two paths share this file:
//   BOARD_HAS_PMU         — AXP2101 over I2C (Waveshare): battery % and
//                            charging status come straight from the PMU,
//                            and the PMU's PKEY drives the middle button.
//   BOARD_HAS_BATTERY_ADC — Plain ADC voltage sense: battery percentage
//                            is estimated from LiPo voltage. Boards may
//                            opt into USB CDC as an external-power hint.
//
// Both paths expose the same power_*() API so ui.cpp doesn't care which
// board it's running on.

#if BOARD_HAS_PMU

#define BATTERY_POLL_MS   2000
#define CHARGING_POLL_MS  500
#define PWR_POLL_MS       50

static int      cached_pct      = -1;
static bool     cached_charging = false;
static bool     pwr_pressed_flag = false;
static uint32_t last_battery_ms  = 0;
static uint32_t last_charging_ms = 0;
static uint32_t last_pwr_ms      = 0;

void power_init(void) {
    if (!pmu.begin(Wire, AXP2101_ADDR, IIC_SDA, IIC_SCL)) {
        Serial.println("AXP2101 init failed");
        return;
    }
    Serial.println("AXP2101 init OK");

    pmu.enableBattDetection();
    pmu.enableBattVoltageMeasure();

    pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    pmu.clearIrqStatus();
    pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);

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

    if (now - last_pwr_ms >= PWR_POLL_MS) {
        last_pwr_ms = now;
        pmu.getIrqStatus();
        if (pmu.isPekeyShortPressIrq()) pwr_pressed_flag = true;
        pmu.clearIrqStatus();
    }
}

int  power_battery_pct(void)   { return cached_pct; }
bool power_is_charging(void)   { return cached_charging; }
bool power_pwr_pressed(void) {
    if (pwr_pressed_flag) { pwr_pressed_flag = false; return true; }
    return false;
}

#elif BOARD_HAS_BATTERY_ADC

#define BAT_POLL_MS       2000
static uint32_t last_bat_ms = 0;
static int      cached_raw  = 0;
static int      cached_pin_mv = 0;
static float    cached_volt = 0.0f;
static int      cached_pct = -1;
static bool     cached_charging = false;

static int voltage_to_percent(float vbat) {
    if (vbat < 2.50f) {
        return -1;  // Battery absent or ADC not connected.
    }

    struct BatteryPoint {
        float volts;
        int percent;
    };

    static const BatteryPoint curve[] = {
        {3.20f, 0},
        {3.50f, 5},
        {3.60f, 10},
        {3.70f, 20},
        {3.75f, 35},
        {3.80f, 50},
        {3.90f, 65},
        {4.00f, 80},
        {4.10f, 90},
        {4.20f, 100},
    };

    if (vbat <= curve[0].volts) return curve[0].percent;

    const size_t last = sizeof(curve) / sizeof(curve[0]) - 1;
    if (vbat >= curve[last].volts) return curve[last].percent;

    for (size_t i = 1; i <= last; ++i) {
        if (vbat <= curve[i].volts) {
            float span = curve[i].volts - curve[i - 1].volts;
            float pos = (vbat - curve[i - 1].volts) / span;
            return curve[i - 1].percent +
                   (int)((curve[i].percent - curve[i - 1].percent) * pos + 0.5f);
        }
    }

    return -1;
}

static bool external_power_present(void) {
#if BAT_CHARGE_DETECT_USB_CDC && defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE && \
    defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
    return HWCDC::isPlugged();
#else
    return false;
#endif
}

static void read_battery(void) {
    cached_raw = analogRead(BAT_ADC_GPIO);
    cached_pin_mv = analogReadMilliVolts(BAT_ADC_GPIO);
    float v_pin = cached_pin_mv > 0
        ? (float)cached_pin_mv / 1000.0f
        : ((float)cached_raw / (float)BAT_ADC_MAX) * BAT_ADC_REF_VOLTAGE;
    cached_volt = v_pin * BAT_ADC_DIVIDER_RATIO * BAT_ADC_CORRECTION_FACTOR;
    cached_pct = voltage_to_percent(cached_volt);
    cached_charging = external_power_present() && cached_pct >= 0;
}

void power_init(void) {
    analogReadResolution(12);
    analogSetPinAttenuation(BAT_ADC_GPIO, ADC_11db);
    pinMode(BAT_ADC_GPIO, INPUT);
    read_battery();
    Serial.printf("BAT ADC init OK (GPIO%d raw=%d pin=%dmV vbat=%.2fV pct=%d usb=%d)\n",
                  BAT_ADC_GPIO, cached_raw, cached_pin_mv, cached_volt,
                  cached_pct, cached_charging ? 1 : 0);
}

void power_tick(void) {
    uint32_t now = millis();
    if (now - last_bat_ms < BAT_POLL_MS) return;
    last_bat_ms = now;
    read_battery();
}

int  power_battery_pct(void) { return cached_pct; }
bool power_is_charging(void) { return cached_charging; }
bool power_pwr_pressed(void) { return false; }

#else

// No battery hardware at all — keep the API alive so callers compile.
void power_init(void)        { Serial.println("power: no battery hardware on this board"); }
void power_tick(void)        {}
int  power_battery_pct(void) { return -1; }
bool power_is_charging(void) { return false; }
bool power_pwr_pressed(void) { return false; }

#endif
