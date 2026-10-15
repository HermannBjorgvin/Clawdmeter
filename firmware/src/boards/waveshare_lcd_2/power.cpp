#include "../../hal/power_hal.h"
#include "board.h"
#include <Arduino.h>

#if BAT_USB_CDC_DETECT && defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE && \
    defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
#include <HWCDC.h>
#define HAVE_USB_PLUG_SENSE 1
#else
#define HAVE_USB_PLUG_SENSE 0
#endif

// No PMU: battery percentage is estimated from the VBAT divider on
// BAT_ADC_PIN (LiPo voltage curve below). The charger status pin is not
// exposed, so "USB plugged in" (the native USB PHY sees VBUS) stands in for
// VBUS and for charging whenever a battery is present. There is no PWR
// button: the pair gesture is the touch hold (caps.has_pwr_button = false).

#define BATTERY_POLL_MS  2000

static int      cached_pct      = -1;
static bool     cached_vbus     = false;
static uint32_t last_battery_ms = 0;

static int voltage_to_percent(float vbat) {
    if (vbat < 2.5f) return -1;   // divider floating: no battery connected
    static const struct { float v; int pct; } curve[] = {
        {3.20f, 0}, {3.50f, 5}, {3.60f, 10}, {3.70f, 20}, {3.75f, 35},
        {3.80f, 50}, {3.90f, 65}, {4.00f, 80}, {4.10f, 90}, {4.20f, 100},
    };
    const size_t last = sizeof(curve) / sizeof(curve[0]) - 1;
    if (vbat <= curve[0].v) return 0;
    if (vbat >= curve[last].v) return 100;
    for (size_t i = 1; i <= last; ++i) {
        if (vbat <= curve[i].v) {
            float pos = (vbat - curve[i - 1].v) / (curve[i].v - curve[i - 1].v);
            return curve[i - 1].pct + (int)((curve[i].pct - curve[i - 1].pct) * pos + 0.5f);
        }
    }
    return -1;
}

static void sample_battery(void) {
    // Average a few reads: the divider is high-impedance and single S3 ADC
    // samples are noisy.
    uint32_t mv = 0;
    for (int i = 0; i < 4; i++) mv += analogReadMilliVolts(BAT_ADC_PIN);
    cached_pct = voltage_to_percent((mv / 4) * BAT_VOLT_DIVIDER / 1000.0f);
#if HAVE_USB_PLUG_SENSE
    cached_vbus = HWCDC::isPlugged();
#endif
}

void power_hal_init(void) {
    analogReadResolution(12);
    analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
    sample_battery();
    Serial.printf("Battery ADC init (GPIO%d) pct=%d usb=%d\n",
                  BAT_ADC_PIN, cached_pct, cached_vbus ? 1 : 0);
}

void power_hal_tick(void) {
    uint32_t now = millis();
    if (now - last_battery_ms < BATTERY_POLL_MS) return;
    last_battery_ms = now;
    sample_battery();
}

int  power_hal_battery_pct(void) { return cached_pct; }
bool power_hal_is_charging(void) { return cached_vbus && cached_pct >= 0; }
bool power_hal_is_vbus_in(void)  { return cached_vbus; }

bool power_hal_pwr_pressed(void)      { return false; }
bool power_hal_pwr_long_pressed(void) { return false; }
bool power_hal_pwr_released(void)     { return false; }
