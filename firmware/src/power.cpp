#include "power.h"
#include "display_cfg.h"
#include <Arduino.h>

// Poll intervals
#define BATTERY_POLL_MS   2000
#define CHARGING_POLL_MS  500

static int      cached_pct       = -1;
static bool     cached_charging  = false;
static bool     pwr_pressed_flag = false;
static uint32_t last_battery_ms  = 0;
static uint32_t last_charging_ms = 0;
static uint32_t last_pwr_ms      = 0;
#define PWR_POLL_MS 50

#if defined(BOARD_WAVESHARE_AMOLED_216)

// ---------------------------------------------------------------------
// AXP2101 implementation (Waveshare).
// XPowersPMU exposes a fuel-gauge percentage directly + a PKEY IRQ for
// the middle button.
// ---------------------------------------------------------------------
void power_early_enable(void) {
    // Waveshare has no PMIC-enable pin — AXP2101 self-powers.
}

void power_init(void) {
    if (!pmu.begin(Wire, PMU_ADDR, IIC_SDA, IIC_SCL)) {
        Serial.println("AXP2101 init failed");
        return;
    }
    Serial.println("AXP2101 init OK");

    pmu.enableBattDetection();
    pmu.enableBattVoltageMeasure();

    // Enable PWR button short-press IRQ
    pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    pmu.clearIrqStatus();
    pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);

    cached_charging = pmu.isCharging();
    cached_pct      = pmu.getBatteryPercent();
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
        if (pmu.isPekeyShortPressIrq()) {
            pwr_pressed_flag = true;
        }
        pmu.clearIrqStatus();
    }
}

#elif defined(BOARD_LILYGO_T4S3)

// ---------------------------------------------------------------------
// SY6970 implementation (LilyGO T4-S3).
// SY6970 is a charger IC, not a fuel gauge — there is no native percent
// reading. Approximate from battery voltage (rough but acceptable for
// a desk indicator). Range is calibrated for a single-cell LiPo:
//   4.20 V → 100 %     3.30 V → 0 %
// ---------------------------------------------------------------------
void power_early_enable(void) {
    static bool done = false;
    if (done) return;
    pinMode(PMU_EN, OUTPUT);
    digitalWrite(PMU_EN, HIGH);
    delay(50);  // give the SY6970 LDOs time to ramp before anything touches them
    done = true;
}

static int approx_pct_from_mv(uint16_t mv) {
    if (mv == 0) return -1;          // no battery present / not yet read
    if (mv >= 4200) return 100;
    if (mv <= 3300) return 0;
    return (int)((mv - 3300) * 100UL / (4200 - 3300));
}

void power_init(void) {
    // PMICEN should already be HIGH from power_early_enable(); ensure it
    // anyway so the function is idempotent if called standalone.
    power_early_enable();

    if (!pmu.init(Wire, IIC_SDA, IIC_SCL, PMU_ADDR)) {
        Serial.println("SY6970 init failed");
        return;
    }
    Serial.println("SY6970 init OK");

    // Enable battery ADC scans (otherwise getBattVoltage returns 0).
    pmu.enableADCMeasure();

    cached_charging = pmu.isCharging();
    cached_pct      = approx_pct_from_mv(pmu.getBattVoltage());
}

void power_tick(void) {
    uint32_t now = millis();

    if (now - last_charging_ms >= CHARGING_POLL_MS) {
        last_charging_ms = now;
        cached_charging = pmu.isCharging();
    }
    if (now - last_battery_ms >= BATTERY_POLL_MS) {
        last_battery_ms = now;
        cached_pct = approx_pct_from_mv(pmu.getBattVoltage());
    }
    // No on-PMU user button.
    (void)last_pwr_ms;
}

#else
#error "power.cpp: no board match"
#endif

int power_battery_pct(void) {
    return cached_pct;
}

bool power_is_charging(void) {
    return cached_charging;
}

bool power_pwr_pressed(void) {
#if HAS_PMU_BUTTON
    if (pwr_pressed_flag) {
        pwr_pressed_flag = false;
        return true;
    }
#endif
    return false;
}
