#include "../../hal/power_hal.h"
#include "board.h"
#include "io_expander.h"
#include "power_state.h"
#include <Arduino.h>
#include <math.h>
#include <Wire.h>
#include <Preferences.h>

// No PMU and no GPIO-wired PWR button — KEY/PWR is EN/RST (hardware reset).
// Hold-to-pair is unavailable; re-pair from the host OS.
//
// Battery: the optional LiPo's voltage comes from the CH32 expander's ADC
// (see io_expander_battery_mv). The charger exposes no readable status to the
// ESP32, so "on USB power" is inferred:
//   - a USB host is attached (the ESP32-S3's USB-Serial-JTAG sees SOF frames)
//     — covers the usual desk case, plugged into the computer; or
//   - the battery voltage is clearly rising (a data-less wall charger).
// Charging = on USB power and not yet full. With no battery connected the
// board can only run from USB, so it reports "no battery" + USB power.

#define BATTERY_POLL_MS    2000
#define TREND_WINDOW_MS    120000UL   // compare against the reading 2 min ago
#define TREND_RISE_MV      25         // rise that counts as "being charged"
#define NO_BATTERY_BELOW_MV 2800      // divider reads ~0 with nothing attached
#define NO_BATTERY_ABOVE_MV 4450      // charger output with no cell present

// Readings are noisy (ADC jitter plus charger ripple), so the voltage is
// smoothed (EMA, ~30 s time constant at a 2 s poll) and the shown percentage
// only moves in the direction the power source allows: up while on USB, down
// on battery. Both reset when the source changes.
#define EMA_ALPHA          0.07f

static bool     sw6106_ok   = false;
static uint32_t sw6106_ms   = 0;

static bool sw6106_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(SW6106_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static int      cached_mv   = -1;     // smoothed
static float    ema_mv      = -1.0f;
static int      shown_pct   = -1;
static int      cached_pct  = -1;
static bool     usb_host    = true;   // assume USB until the first sample
static bool     rising      = false;
// Charger STAT: 1 charging, 0 not, -1 unknown. Only meaningful while USB
// power is present — on battery the ETA6098 leaves it floating, and it reads
// as "charging". So it refines charging-vs-full on USB, never detects USB.
static int      chg_stat    = -1;
static uint32_t last_poll_ms = 0;
static int      trend_mv    = -1;
static uint32_t trend_ms    = 0;

// LiPo open-circuit curve (mV → %), linear between points.
static int pct_from_mv(int mv) {
    static const int16_t pts[][2] = {
        {4200, 100}, {4100, 90}, {4000, 80}, {3900, 65}, {3800, 50},
        {3750, 40},  {3700, 30}, {3650, 20}, {3600, 12}, {3500, 5}, {3300, 0},
    };
    const int n = sizeof(pts) / sizeof(pts[0]);
    if (mv >= pts[0][0]) return 100;
    for (int i = 1; i < n; i++) {
        if (mv >= pts[i][0]) {
            int dv = pts[i - 1][0] - pts[i][0];
            int dp = pts[i - 1][1] - pts[i][1];
            return pts[i][1] + (mv - pts[i][0]) * dp / dv;
        }
    }
    return 0;
}

// Voltage only tells the charge level when no current is flowing. While
// charging it reads high: ~70 mV in the constant-current phase (measured on
// this board: 93% plugged vs 87% unplugged), and in the constant-voltage
// phase (~4.15 V+) the charger pins it near 4.2 V whatever the real level.
// So on USB:
//   - start from the last on-battery percentage (saved to flash, since this
//     board resets when USB is plugged in),
//   - raise it only from constant-current readings, less the 70 mV rise,
//   - in the constant-voltage phase the cell is past ~90%: start there and
//     step 1% per CV_STEP_MS up to 100 (the charger's taper).
// Progress is saved as it rises, so the board's resets don't restart it.
// The number is conservative while charging and right once unplugged.
#define CHARGE_RISE_MV     70
#define CV_START_MV        4150
#define CV_FLOOR_PCT       90
#define CV_STEP_MS         (2UL * 60UL * 1000UL)

static int      saved_pct   = -1;     // last known %, persisted
static uint32_t cv_step_ms  = 0;      // 0 = not in the constant-voltage phase

static void save_pct(int pct) {
    if (pct == saved_pct) return;
    saved_pct = pct;
    Preferences prefs;
    prefs.begin("lcd4bat", false);
    prefs.putChar("pct", (int8_t)pct);
    prefs.end();
}

// Percentage while on USB power (charging).
static int charging_pct(int mv, uint32_t now) {
    int pct = saved_pct;
    if (mv < CV_START_MV) {
        cv_step_ms = 0;
        int est = pct_from_mv(mv - CHARGE_RISE_MV);
        if (est > pct) pct = est;
    } else {
        if (pct < CV_FLOOR_PCT) pct = CV_FLOOR_PCT;
        if (cv_step_ms == 0) cv_step_ms = now ? now : 1;
        else if (now - cv_step_ms >= CV_STEP_MS && pct < 100) { pct++; cv_step_ms = now; }
    }
    save_pct(pct);
    return pct;
}

static bool battery_present(void) {
    return cached_mv >= NO_BATTERY_BELOW_MV && cached_mv <= NO_BATTERY_ABOVE_MV;
}

static void sample(void) {
    uint32_t now = millis();
    bool was_on_battery = lcd4_on_battery();

    int raw_mv = io_expander_battery_mv();
    usb_host   = HWCDC::isPlugged();
    chg_stat   = io_expander_charging();
    if (raw_mv < 0) return;   // read failed — keep the last values
    // Seed on the first reading or after a big jump (battery plugged/unplugged).
    if (ema_mv < 0 || fabsf(raw_mv - ema_mv) > 400.0f) ema_mv = raw_mv;
    else                                                ema_mv += EMA_ALPHA * (raw_mv - ema_mv);
    cached_mv = (int)(ema_mv + 0.5f);

    if (!battery_present()) {
        cached_pct = shown_pct = -1;
    } else {
        bool on_usb = usb_host || rising;
        int pct = on_usb ? charging_pct(cached_mv, now) : pct_from_mv(cached_mv);
        if (shown_pct < 0)          shown_pct = pct;
        else if (on_usb  && pct > shown_pct) shown_pct = pct;   // charging: only up
        else if (!on_usb && pct < shown_pct) shown_pct = pct;   // battery: only down
        cached_pct = shown_pct;
        if (!on_usb) save_pct(shown_pct);   // resting reading — the one to trust
    }

    // Voltage trend for data-less chargers.
    if (battery_present()) {
        if (trend_mv < 0) { trend_mv = cached_mv; trend_ms = now; }
        if (now - trend_ms >= TREND_WINDOW_MS) {
            rising = cached_mv - trend_mv >= TREND_RISE_MV;
            trend_mv = cached_mv;
            trend_ms = now;
        }
    } else {
        trend_mv = -1;
        rising = false;
    }

    static uint32_t last_log_ms = 0;
    static bool first = true;
    bool changed = lcd4_on_battery() != was_on_battery;
    // New power source: re-settle (on battery the voltage relaxes over ~30 s;
    // the only-down rule then walks the number down to it).
    if (changed && battery_present()) {
        cv_step_ms = 0;
        cached_pct = shown_pct = lcd4_on_battery() ? pct_from_mv(cached_mv)
                                                   : charging_pct(cached_mv, now);
    }
    if (first || changed || now - last_log_ms >= 60000) {
        first = false;
        last_log_ms = now;
        Serial.printf("Battery: %d mV (%d%%) usb_host=%d rising=%d stat=%d -> %s%s\n",
                      cached_mv, cached_pct, usb_host, rising, chg_stat,
                      lcd4_on_battery() ? "on battery" : "USB power",
                      power_hal_is_charging() ? ", charging" : "");
    }
    if (changed) lcd4_reapply_brightness();
}

bool lcd4_on_battery(void) {
    return battery_present() && !usb_host && !rising;
}

void power_hal_init(void) {
    Preferences prefs;
    prefs.begin("lcd4bat", true);
    saved_pct = prefs.getChar("pct", -1);
    prefs.end();
    // Stop the charger cutting battery power under light load (see board.h).
    sw6106_ok = sw6106_write(SW6106_REG_LIGHTLOAD, SW6106_LIGHTLOAD_OFF);
    // Not every revision has it on I2C (CH32V003 boards don't) — then the
    // battery boost is started by the PWR key and held by SYS_EN instead.
    Serial.printf("SW6106 @ 0x%02X: %s\n", SW6106_ADDR,
                  sw6106_ok ? "light-load shutdown disabled" : "not on I2C (PWR key starts battery power)");
    sw6106_ms = millis();
    sample();
    last_poll_ms = millis();
}

void power_hal_tick(void) {
    if (sw6106_ok && millis() - sw6106_ms >= SW6106_KEEPALIVE_MS) {
        sw6106_ms = millis();
        sw6106_write(SW6106_REG_KEEPALIVE, SW6106_KEEPALIVE);
    }
    if (millis() - last_poll_ms >= BATTERY_POLL_MS) {
        last_poll_ms = millis();
        sample();
    }
}

int  power_hal_battery_pct(void) { return cached_pct; }
bool power_hal_is_vbus_in(void)  { return !lcd4_on_battery(); }
bool power_hal_is_charging(void) {
    if (!battery_present() || lcd4_on_battery()) return false;
    if (chg_stat >= 0) return chg_stat == 1;   // on USB: the charger knows charging vs full
    // On USB: charging until full. Voltage alone can't say (the constant-voltage
    // phase sits near 4.2 V while still charging), so "full" is the same call the
    // percentage makes — 100% once the CV-phase ramp tops out.
    return cached_pct < 100;
}
bool power_hal_pwr_pressed(void) { return false; }
bool power_hal_pwr_long_pressed(void) { return false; }
bool power_hal_pwr_released(void) { return false; }
