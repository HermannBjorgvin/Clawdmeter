#include "../../hal/power_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// Battery + charging come from the IP5306 power-bank IC over I2C. The IP5306
// only exposes a coarse 4-LED fuel gauge (25/50/75/100 %), which is plenty for
// the four-state battery icon. Hardware power on/off is handled by the IP5306
// itself via M5Stack's red side button, so — unlike the LCD-1.54 — this port
// never synthesizes a software power-off.
//
// The PWR-role button is the middle front button (B) on a plain input-only
// GPIO. Same software edge synthesis as the other button-only ports so the
// hold-to-pair gesture in main.cpp stays board-agnostic:
//   short    — fired on release if the hold was shorter than PWR_LONG_MS
//   long     — fired once when a hold crosses PWR_LONG_MS
//   release  — fired on every release edge

#define BATTERY_POLL_MS  2000
#define PWR_POLL_MS      50
#define PWR_LONG_MS      1500

static int      cached_pct        = -1;
static bool     cached_charging   = false;
static bool     pwr_pressed_flag  = false;
static bool     pwr_long_flag     = false;
static bool     pwr_released_flag = false;
static bool     last_pwr_state    = false;
static uint32_t pwr_press_started_ms = 0;
static bool     pwr_long_fired    = false;
static uint32_t last_battery_ms   = 0;
static uint32_t last_pwr_ms       = 0;

// Read one IP5306 register; returns false (and leaves *out untouched) on a bus
// error so a hiccup doesn't get misread as 0x00 (which the gauge maps to 100%).
static bool ip5306_read(uint8_t reg, uint8_t* out) {
    Wire.beginTransmission(IP5306_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((uint8_t)IP5306_ADDR, (uint8_t)1) != 1) return false;
    *out = Wire.read();
    return true;
}

static void sample_battery(void) {
    uint8_t gauge;
    if (!ip5306_read(0x78, &gauge)) {   // REG_READ4: 4-LED fuel gauge in the high nibble
        cached_pct = -1;
        return;
    }
    // Thermometer nibble: more high bits set = lower charge.
    switch (gauge & 0xF0) {
    case 0x00: cached_pct = 100; break;
    case 0x80: cached_pct = 75;  break;
    case 0xC0: cached_pct = 50;  break;
    case 0xE0: cached_pct = 25;  break;
    default:   cached_pct = 5;   break;   // 0xF0 — nearly empty
    }

    // REG_READ0 bit3 = charging in progress; REG_READ1 bit3 = charge-full. Show
    // the bolt while either is true (i.e. USB is supplying the pack). These bit
    // meanings are from community IP5306 notes — verify the bolt on hardware and
    // flip the masks here if it reads inverted.
    uint8_t r0 = 0, r1 = 0;
    bool ok0 = ip5306_read(0x70, &r0);
    bool ok1 = ip5306_read(0x71, &r1);
    cached_charging = (ok0 && (r0 & 0x08)) || (ok1 && (r1 & 0x08));
}

void power_hal_init(void) {
    pinMode(BTN_B_GPIO, INPUT);   // input-only GPIO, external pull-up on M5Stack
    sample_battery();
}

void power_hal_tick(void) {
    uint32_t now = millis();

    if (now - last_battery_ms >= BATTERY_POLL_MS) {
        last_battery_ms = now;
        sample_battery();
    }
    if (now - last_pwr_ms >= PWR_POLL_MS) {
        last_pwr_ms = now;
        bool pwr_now = (digitalRead(BTN_B_GPIO) == LOW);   // active LOW
        if (pwr_now && !last_pwr_state) {            // press edge — hold begins
            pwr_press_started_ms = now;
            pwr_long_fired = false;
        } else if (pwr_now && last_pwr_state) {      // held
            if (!pwr_long_fired && (now - pwr_press_started_ms >= PWR_LONG_MS)) {
                pwr_long_flag  = true;
                pwr_long_fired = true;
            }
        } else if (!pwr_now && last_pwr_state) {     // release edge
            pwr_released_flag = true;
            if (!pwr_long_fired) pwr_pressed_flag = true;  // short press
        }
        last_pwr_state = pwr_now;
    }
}

int  power_hal_battery_pct(void) { return cached_pct; }
bool power_hal_is_charging(void) { return cached_charging; }
bool power_hal_is_vbus_in(void)  { return cached_charging; }

bool power_hal_pwr_pressed(void) {
    if (pwr_pressed_flag) { pwr_pressed_flag = false; return true; }
    return false;
}

bool power_hal_pwr_long_pressed(void) {
    if (pwr_long_flag) { pwr_long_flag = false; return true; }
    return false;
}

bool power_hal_pwr_released(void) {
    if (pwr_released_flag) { pwr_released_flag = false; return true; }
    return false;
}
