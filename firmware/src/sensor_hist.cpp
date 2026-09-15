#include "sensor_hist.h"
#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>

namespace {

#define SH_SLOT_MS   ((uint32_t)SH_SLOT_SECS * 1000UL)
#define SH_MAGIC     0x53483031UL   // "SH01"
#define SH_NONE      INT16_MIN      // per-field "no reading" sentinel
#define SH_P_BASE    800.0f         // pressure is stored as (hPa - 800) x10

// 6 bytes/slot; 96 slots -> 576 bytes of samples.
struct Slot {
    int16_t t_dc;      // temp     x10 degC
    int16_t h_dpct;    // humidity x10 %
    int16_t p_dhpa;    // pressure x10 hPa, offset by SH_P_BASE
};

struct Blob {
    uint32_t magic;
    int32_t  head_bucket;  // absolute 15-min bucket number of slot[head]
    uint16_t head;         // ring index of the newest (in-progress) slot
    uint8_t  real_time;    // head_bucket is an epoch bucket, not a millis one
    uint8_t  rsv;
    Slot     slot[SH_SLOTS];
};

// This layout is the on-flash NVS format: keep these assertions honest or an
// older device's stored ring will be misread after an update. The magic and
// the length check in sensor_hist_init() catch a genuine format change; these
// catch an accidental one.
static_assert(sizeof(Slot) == 6,   "Slot layout changed - bump SH_MAGIC");
static_assert(sizeof(Blob) == 588, "Blob layout changed - bump SH_MAGIC");

Blob g;                    // 588 bytes — the ring itself

// Clock anchor. Buckets advance as millis() runs; sensor_hist_set_time()
// re-pins the anchor to the daemon's wall clock whenever a payload arrives.
int32_t  anchor_bucket = 0;
uint32_t ref_millis    = 0;
bool     real_time     = false;   // anchor is wall-clock, not free-running
bool     saved_real    = false;   // the restored ring was wall-clock indexed

// In-progress slot accumulators (running mean written straight into slot[head]).
float    acc[SH_METRIC_COUNT];
uint16_t acc_n[SH_METRIC_COUNT];

const Slot EMPTY_SLOT = { SH_NONE, SH_NONE, SH_NONE };

void reset_acc(void) {
    for (int i = 0; i < SH_METRIC_COUNT; i++) { acc[i] = 0; acc_n[i] = 0; }
}

void clear_all(int32_t bucket) {
    for (int i = 0; i < SH_SLOTS; i++) g.slot[i] = EMPTY_SLOT;
    g.head = 0;
    g.head_bucket = bucket;
    reset_acc();
}

void persist(void) {
    g.magic     = SH_MAGIC;
    g.real_time = real_time ? 1 : 0;
    Preferences p;
    if (!p.begin("shist", false)) return;
    p.putBytes("ring", &g, sizeof(g));
    p.end();
}

// Roll the ring forward to `bucket`, blanking every slot we skipped over.
void roll_to(int32_t bucket) {
    int32_t delta = bucket - g.head_bucket;
    if (delta == 0) return;
    if (delta < 0 || delta >= SH_SLOTS) {   // clock jumped, or we were off > 24 h
        clear_all(bucket);
        return;
    }
    for (int32_t i = 0; i < delta; i++) {
        g.head = (uint16_t)((g.head + 1) % SH_SLOTS);
        g.slot[g.head] = EMPTY_SLOT;
    }
    g.head_bucket = bucket;
    reset_acc();
}

int16_t enc(sh_metric_t m, float v) {
    float scaled = (m == SH_PRESS) ? (v - SH_P_BASE) * 10.0f : v * 10.0f;
    if (!(scaled > -32000.0f && scaled < 32000.0f)) return SH_NONE;  // also traps NaN
    return (int16_t)lroundf(scaled);
}

float dec(sh_metric_t m, int16_t raw) {
    if (raw == SH_NONE) return NAN;
    return (m == SH_PRESS) ? raw / 10.0f + SH_P_BASE : raw / 10.0f;
}

int16_t* field(Slot* s, sh_metric_t m) {
    switch (m) {
    case SH_TEMP:  return &s->t_dc;
    case SH_HUM:   return &s->h_dpct;
    default:       return &s->p_dhpa;
    }
}

}  // namespace

void sensor_hist_init(void) {
    reset_acc();
    bool restored = false;

    Preferences p;
    // Read-write, not read-only: opening a namespace that has never been
    // created before in read-only mode crashes on this esp32-arduino version
    // (assert failed: xQueueSemaphoreTake, right after the expected nvs_open
    // NOT_FOUND) -- read-write auto-creates the namespace instead. Only
    // matters on a truly fresh device (namespace already exists otherwise).
    if (p.begin("shist", false)) {
        if (p.getBytesLength("ring") == sizeof(Blob)) {
            Blob tmp;
            if (p.getBytes("ring", &tmp, sizeof(tmp)) == sizeof(tmp) &&
                tmp.magic == SH_MAGIC && tmp.head < SH_SLOTS) {
                g = tmp;
                saved_real = tmp.real_time != 0;
                restored = true;
            }
        }
        p.end();
    }

    if (!restored) clear_all(0);

    // No wall clock yet: keep counting buckets from wherever the restored ring
    // left off. If that ring was wall-clock indexed we stay in the same
    // absolute space (just missing however long we were powered down, which
    // the first sensor_hist_set_time() corrects). If it wasn't, the first real
    // clock re-pins the whole trace instead — see sensor_hist_set_time().
    anchor_bucket = g.head_bucket;
    ref_millis    = millis();
    real_time     = false;

    Serial.printf("sensor hist: %s (%d samples, %d min)\n",
                  restored ? "restored" : "empty",
                  sensor_hist_series(SH_TEMP, nullptr, 0),
                  sensor_hist_span_mins());
}

void sensor_hist_set_time(long epoch_utc) {
    if (epoch_utc < 1000000000L) return;   // obviously bogus, ignore

    int32_t  b       = (int32_t)(epoch_utc / SH_SLOT_SECS);
    uint32_t into_ms = (uint32_t)(epoch_utc % SH_SLOT_SECS) * 1000UL;

    // Unsigned wrap here is intentional and harmless: only (millis() -
    // ref_millis) is ever evaluated, so an early-boot underflow still yields
    // the correct offset into the current bucket.
    anchor_bucket = b;
    ref_millis    = millis() - into_ms;

    if (!real_time) {
        real_time = true;
        // A ring saved without a wall clock has a meaningless bucket number.
        // Its samples are still on a correct 15-minute cadence though, so pin
        // its newest slot to "now" rather than throwing the trace away.
        if (!saved_real) g.head_bucket = b;
        saved_real = true;
    }

    if (b != g.head_bucket) { roll_to(b); persist(); }
}

void sensor_hist_tick(void) {
    // Advance the anchor incrementally so the millis() rollover at ~49 days
    // never produces a bogus delta.
    uint32_t elapsed = millis() - ref_millis;
    if (elapsed >= SH_SLOT_MS) {
        uint32_t n = elapsed / SH_SLOT_MS;
        anchor_bucket += (int32_t)n;
        ref_millis    += n * SH_SLOT_MS;
    }
    if (anchor_bucket != g.head_bucket) {
        roll_to(anchor_bucket);
        persist();
    }
}

void sensor_hist_sample(float temp_c, bool has_hum, float hum_pct, float press_hpa) {
    Slot* s = &g.slot[g.head];

    struct { sh_metric_t m; float v; bool use; } in[] = {
        { SH_TEMP,  temp_c,    true    },
        { SH_HUM,   hum_pct,   has_hum },
        { SH_PRESS, press_hpa, true    },
    };

    for (unsigned i = 0; i < sizeof(in) / sizeof(in[0]); i++) {
        if (!in[i].use || isnan(in[i].v)) continue;
        int k = (int)in[i].m;
        acc[k] += in[i].v;
        acc_n[k]++;
        *field(s, in[i].m) = enc(in[i].m, acc[k] / acc_n[k]);
    }
}

int sensor_hist_series(sh_metric_t m, float* out, int n) {
    int valid = 0;
    for (int i = 0; i < SH_SLOTS; i++) {
        // slot[head] is the newest, so the oldest sits one past it.
        int idx = (g.head + 1 + i) % SH_SLOTS;
        float v = dec(m, *field(&g.slot[idx], m));
        if (!isnan(v)) valid++;
        if (out && i < n) out[i] = v;
    }
    return valid;
}

int sensor_hist_span_mins(void) {
    for (int i = 0; i < SH_SLOTS; i++) {
        int idx = (g.head + 1 + i) % SH_SLOTS;
        const Slot* s = &g.slot[idx];
        if (s->t_dc != SH_NONE || s->h_dpct != SH_NONE || s->p_dhpa != SH_NONE)
            return (SH_SLOTS - i) * (SH_SLOT_SECS / 60);
    }
    return 0;
}

bool sensor_hist_time_is_real(void) { return real_time; }

void sensor_hist_clear(void) {
    clear_all(anchor_bucket);
    persist();
}

void sensor_hist_debug_fill(void) {
    clear_all(anchor_bucket);
    for (int i = 0; i < SH_SLOTS; i++) {
        // i = 0 is 24 h ago, i = SH_SLOTS-1 is now.
        float hours = (float)i * (SH_SLOT_SECS / 3600.0f);
        float diurnal = sinf((hours - 4.0f) / 24.0f * 2.0f * (float)M_PI);
        Slot* s = &g.slot[i];
        s->t_dc   = enc(SH_TEMP,  21.5f + 2.8f * diurnal + 0.15f * sinf(hours * 3.1f));
        s->h_dpct = enc(SH_HUM,   46.0f - 9.0f * diurnal + 0.8f  * sinf(hours * 2.3f));
        s->p_dhpa = enc(SH_PRESS, 1012.0f + 3.5f * sinf(hours / 19.0f * 2.0f * (float)M_PI));
    }
    g.head = SH_SLOTS - 1;
    reset_acc();
    persist();
    Serial.println("sensor hist: filled with synthetic 24 h data");
}
