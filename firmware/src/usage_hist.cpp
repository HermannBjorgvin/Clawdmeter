#include "usage_hist.h"
#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>

namespace {

#define UH_SLOT_MS   ((uint32_t)UH_SLOT_SECS * 1000UL)
#define UH_MAGIC     0x55483031UL   // "UH01"
#define UH_NONE      255            // percentages are 0-100; 255 = no reading

// 2 bytes/slot; 96 slots -> 192 bytes of samples.
struct Slot {
    uint8_t session;   // %
    uint8_t weekly;    // %
};

struct Blob {
    uint32_t magic;
    int32_t  head_bucket;  // absolute 15-min bucket number of slot[head]
    uint16_t head;         // ring index of the newest (in-progress) slot
    uint8_t  real_time;    // head_bucket is an epoch bucket, not a millis one
    uint8_t  rsv;
    Slot     slot[UH_SLOTS];
};

static_assert(sizeof(Slot) == 2,   "Slot layout changed - bump UH_MAGIC");
static_assert(sizeof(Blob) == 204, "Blob layout changed - bump UH_MAGIC");

Blob g;                    // 204 bytes — the ring itself

// Clock anchor — identical scheme to sensor_hist.cpp. usage_hist_set_time()
// is fed from the same {"src":"env"} payload, so the two rings stay in sync.
int32_t  anchor_bucket = 0;
uint32_t ref_millis    = 0;
bool     real_time     = false;
bool     saved_real    = false;

float    acc[UH_METRIC_COUNT];
uint16_t acc_n[UH_METRIC_COUNT];

const Slot EMPTY_SLOT = { UH_NONE, UH_NONE };

void reset_acc(void) {
    for (int i = 0; i < UH_METRIC_COUNT; i++) { acc[i] = 0; acc_n[i] = 0; }
}

void clear_all(int32_t bucket) {
    for (int i = 0; i < UH_SLOTS; i++) g.slot[i] = EMPTY_SLOT;
    g.head = 0;
    g.head_bucket = bucket;
    reset_acc();
}

void persist(void) {
    g.magic     = UH_MAGIC;
    g.real_time = real_time ? 1 : 0;
    Preferences p;
    if (!p.begin("uhist", false)) return;
    p.putBytes("ring", &g, sizeof(g));
    p.end();
}

void roll_to(int32_t bucket) {
    int32_t delta = bucket - g.head_bucket;
    if (delta == 0) return;
    if (delta < 0 || delta >= UH_SLOTS) {
        clear_all(bucket);
        return;
    }
    for (int32_t i = 0; i < delta; i++) {
        g.head = (uint16_t)((g.head + 1) % UH_SLOTS);
        g.slot[g.head] = EMPTY_SLOT;
    }
    g.head_bucket = bucket;
    reset_acc();
}

uint8_t enc(float v) {
    if (isnan(v) || v < 0.0f) return UH_NONE;
    if (v > 100.0f) v = 100.0f;
    return (uint8_t)lroundf(v);
}

float dec(uint8_t raw) {
    return (raw == UH_NONE) ? NAN : (float)raw;
}

uint8_t* field(Slot* s, uh_metric_t m) {
    return (m == UH_SESSION) ? &s->session : &s->weekly;
}

}  // namespace

void usage_hist_init(void) {
    reset_acc();
    bool restored = false;

    Preferences p;
    // Read-write, not read-only: opening a namespace that has never been
    // created before in read-only mode crashes on this esp32-arduino version
    // (assert failed: xQueueSemaphoreTake, right after the expected nvs_open
    // NOT_FOUND) -- read-write auto-creates the namespace instead. Only
    // matters on a truly fresh device (namespace already exists otherwise).
    if (p.begin("uhist", false)) {
        if (p.getBytesLength("ring") == sizeof(Blob)) {
            Blob tmp;
            if (p.getBytes("ring", &tmp, sizeof(tmp)) == sizeof(tmp) &&
                tmp.magic == UH_MAGIC && tmp.head < UH_SLOTS) {
                g = tmp;
                saved_real = tmp.real_time != 0;
                restored = true;
            }
        }
        p.end();
    }

    if (!restored) clear_all(0);

    anchor_bucket = g.head_bucket;
    ref_millis    = millis();
    real_time     = false;

    Serial.printf("usage hist: %s (%d samples)\n",
                  restored ? "restored" : "empty",
                  usage_hist_series(UH_SESSION, nullptr, 0));
}

void usage_hist_set_time(long epoch_utc) {
    if (epoch_utc < 1000000000L) return;   // obviously bogus, ignore

    int32_t  b       = (int32_t)(epoch_utc / UH_SLOT_SECS);
    uint32_t into_ms = (uint32_t)(epoch_utc % UH_SLOT_SECS) * 1000UL;

    anchor_bucket = b;
    ref_millis    = millis() - into_ms;

    if (!real_time) {
        real_time = true;
        if (!saved_real) g.head_bucket = b;
        saved_real = true;
    }

    if (b != g.head_bucket) { roll_to(b); persist(); }
}

void usage_hist_tick(void) {
    uint32_t elapsed = millis() - ref_millis;
    if (elapsed >= UH_SLOT_MS) {
        uint32_t n = elapsed / UH_SLOT_MS;
        anchor_bucket += (int32_t)n;
        ref_millis    += n * UH_SLOT_MS;
    }
    if (anchor_bucket != g.head_bucket) {
        roll_to(anchor_bucket);
        persist();
    }
}

void usage_hist_sample(float session_pct, float weekly_pct) {
    Slot* s = &g.slot[g.head];

    struct { uh_metric_t m; float v; } in[] = {
        { UH_SESSION, session_pct },
        { UH_WEEKLY,  weekly_pct  },
    };

    for (unsigned i = 0; i < sizeof(in) / sizeof(in[0]); i++) {
        if (isnan(in[i].v)) continue;
        int k = (int)in[i].m;
        acc[k] += in[i].v;
        acc_n[k]++;
        *field(s, in[i].m) = enc(acc[k] / acc_n[k]);
    }
}

int usage_hist_series(uh_metric_t m, float* out, int n) {
    int valid = 0;
    for (int i = 0; i < UH_SLOTS; i++) {
        int idx = (g.head + 1 + i) % UH_SLOTS;   // oldest sits one past head
        float v = dec(*field(&g.slot[idx], m));
        if (!isnan(v)) valid++;
        if (out && i < n) out[i] = v;
    }
    return valid;
}

void usage_hist_clear(void) {
    clear_all(anchor_bucket);
    persist();
}

void usage_hist_debug_fill(void) {
    clear_all(anchor_bucket);
    for (int i = 0; i < UH_SLOTS; i++) {
        // i = 0 is 24 h ago, i = UH_SLOTS-1 is now. Session ramps up across a
        // handful of resets/day; weekly climbs steadily toward the 7-day cap.
        float hours = (float)i * (UH_SLOT_SECS / 3600.0f);
        float session = fmodf(hours, 5.0f) / 5.0f * 100.0f;
        float weekly  = 30.0f + hours * 1.5f;
        if (weekly > 100.0f) weekly = 100.0f;
        g.slot[i].session = enc(session);
        g.slot[i].weekly  = enc(weekly);
    }
    g.head = UH_SLOTS - 1;
    reset_acc();
    persist();
    Serial.println("usage hist: filled with synthetic 24 h data");
}
