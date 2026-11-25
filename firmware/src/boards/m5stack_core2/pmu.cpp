#include "pmu.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>

static PmuKind kind = PMU_NONE;

static bool rd(uint8_t reg, uint8_t* out) {
    Wire.beginTransmission(PMU_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((uint8_t)PMU_ADDR, (uint8_t)1) != 1) return false;
    *out = Wire.read();
    return true;
}

static bool wr(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(PMU_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static void bits(uint8_t reg, uint8_t set, uint8_t clear) {
    uint8_t v;
    if (rd(reg, &v)) wr(reg, (uint8_t)((v & ~clear) | set));
}

// ---------------------------------------------------------------- AXP2101 (v1.1)
// 0x90 LDO enable: b0 ALDO1, b1 ALDO2 (panel reset line), b2 ALDO3 (speaker amp),
// b3 ALDO4 (panel / microSD power), b4 BLDO1 (backlight), b5 BLDO2, b7 DLDO1
// (vibration motor). LDO voltage registers: 0.5 V + 0.1 V * n.
static void axp2101_init(void) {
    wr(0x93, 33 - 5);              // ALDO2 3.3 V
    wr(0x94, 33 - 5);              // ALDO3 3.3 V (speaker amp enable, off for now)
    wr(0x95, 33 - 5);              // ALDO4 3.3 V
    wr(0x96, 28 - 5);              // BLDO1 2.8 V until the brightness is set
    wr(0x97, 33 - 5);              // BLDO2 3.3 V
    // Rails on, vibration motor and speaker amp off.
    bits(0x90, 0x3A, 0x84);
    wr(0x68, 0x01);                // battery detection on
    bits(0x30, 0x0F, 0);           // ADCs on (battery / VBUS / system voltage)
    // Panel reset: ALDO2 low, then high (as M5GFX does on the v1.1).
    bits(0x90, 0, 0x02);
    delay(20);
    bits(0x90, 0x02, 0);
    delay(120);
}

static void axp2101_backlight(uint8_t level) {
    if (level == 0) { bits(0x90, 0, 0x10); return; }
    wr(0x96, (uint8_t)((level + 641) >> 5));   // 20..28 -> 2.5..3.3 V
    bits(0x90, 0x10, 0);
}

// ----------------------------------------------------------------- AXP192 (v1.0)
// 0x12 output enable: b0 DCDC1 (ESP32), b1 DCDC3 (backlight), b2 LDO2 (panel
// logic + microSD), b3 LDO3 (vibration motor). GPIO4 (0x95/0x96) is the panel reset.
static void axp192_init(void) {
    bits(0x30, 0x02, 0xFB);                    // VBUS-IPSOUT path, as M5Core2 sets it
    uint8_t ldo;
    if (rd(0x28, &ldo)) wr(0x28, (uint8_t)((ldo & 0x0F) | (15 << 4)));   // LDO2 3.3 V
    wr(0x27, (uint8_t)((2800 - 700) / 25));    // DCDC3 2.8 V
    bits(0x12, 0x06, 0x08);                    // DCDC3 + LDO2 on, vibration off
    // 5 V bus (M-Bus / speaker supply on battery), as M5Unified's Core2
    // setExtOutput(true) and M5Core2's SetBusPowerMode(0) do: GPIO0 as a 3.3 V
    // LDO, then EXTEN (0x12 b6). The v1.1's equivalent is BLDO2 (axp2101_init).
    // Untested on v1.0 hardware.
    bits(0x91, 0xF0, 0xF0);
    wr(0x90, 0x02);
    bits(0x12, 0x40, 0);
    wr(0x82, 0xFF);                            // ADCs on
    // GPIO2 (speaker amp enable) as an NMOS open-drain output, low = amp off.
    bits(0x93, 0, 0x07);
    bits(0x94, 0, 0x04);
    // GPIO4 as an NMOS open-drain output, then pulse it low.
    uint8_t g;
    if (rd(0x95, &g)) wr(0x95, (uint8_t)((g & 0x72) | 0x84));
    bits(0x96, 0, 0x02);
    delay(100);
    bits(0x96, 0x02, 0);
    delay(100);
}

static void axp192_backlight(uint8_t level) {
    if (level == 0) { bits(0x12, 0, 0x02); return; }
    uint16_t mv = 2500 + (uint16_t)((uint32_t)level * 800 / 255);
    wr(0x27, (uint8_t)((mv - 700) / 25));
    bits(0x12, 0x02, 0);
}

// ------------------------------------------------------------------------ API
PmuKind pmu_init(void) {
    uint8_t id = 0;
    if (!rd(0x03, &id)) {
        Serial.println("Core2: no power chip at 0x34");
        kind = PMU_NONE;
        return kind;
    }
    kind = (id == 0x4A) ? PMU_AXP2101 : PMU_AXP192;
    Serial.printf("Core2: %s (id 0x%02X)\n", pmu_name(), id);
    if (kind == PMU_AXP2101) axp2101_init();
    else                     axp192_init();
    return kind;
}

PmuKind pmu_kind(void) { return kind; }

const char* pmu_name(void) {
    switch (kind) {
    case PMU_AXP2101: return "AXP2101 (v1.1)";
    case PMU_AXP192:  return "AXP192 (v1.0)";
    default:          return "none";
    }
}

void pmu_set_backlight(uint8_t level) {
    if (kind == PMU_AXP2101)     axp2101_backlight(level);
    else if (kind == PMU_AXP192) axp192_backlight(level);
}

void pmu_set_speaker(bool on) {
    if (kind == PMU_AXP2101)     bits(0x90, on ? 0x04 : 0, on ? 0 : 0x04);   // ALDO3
    else if (kind == PMU_AXP192) bits(0x94, on ? 0x04 : 0, on ? 0 : 0x04);   // GPIO2
}

int pmu_battery_pct(void) {
    uint8_t v;
    if (kind == PMU_AXP2101) {
        if (!rd(0x00, &v) || !(v & 0x08)) return -1;   // b3: battery present
        if (!rd(0xA4, &v)) return -1;                   // fuel gauge %
        return v > 100 ? 100 : v;
    }
    if (kind == PMU_AXP192) {
        if (!rd(0x01, &v) || !(v & 0x20)) return -1;   // b5: battery present
        uint8_t hi, lo;
        if (!rd(0x78, &hi) || !rd(0x79, &lo)) return -1;
        // No fuel gauge: a linear 3.3 .. 4.15 V estimate (1.1 mV per LSB).
        int mv = (int)((((uint16_t)hi << 4) | (lo & 0x0F)) * 11 / 10);
        int pct = (mv - 3300) * 100 / (4150 - 3300);
        return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    }
    return -1;
}

bool pmu_is_charging(void) {
    uint8_t v;
    if (kind == PMU_AXP2101) {
        // STATUS2 b6:5 is the battery current direction: 01 charging,
        // 10 discharging, 00 standby (as XPowersLib's isCharging reads it).
        return rd(0x01, &v) && ((v >> 5) & 0x03) == 0x01;
    }
    if (kind == PMU_AXP192) return rd(0x01, &v) && (v & 0x40);
    return false;
}

bool pmu_is_vbus_in(void) {
    uint8_t v;
    if (kind == PMU_AXP2101) return rd(0x00, &v) && (v & 0x20);
    if (kind == PMU_AXP192)  return rd(0x00, &v) && (v & 0xA0);   // ACIN or VBUS
    return false;
}
