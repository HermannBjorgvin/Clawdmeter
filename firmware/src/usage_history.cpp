#include "usage_history.h"
#include <math.h>
#include <Preferences.h>

// Same ~4-minute trust window as usage_rate.cpp: below this span a 1% blip
// between two 60s samples would read as a wild slope. Below it the rate helpers
// report "not ready" so the burn page shows a warm-up message instead of noise.
#define HIST_MIN_WINDOW_MS 240000UL

// A drop of more than this (percentage points) between consecutive samples is a
// window reset (5h session rollover or the weekly reset), not real burn — the
// rate window restarts there so a reset never shows as negative/again-climbing.
#define RESET_DROP_PP 5.0f

static UsageHistPoint ring[USAGE_HIST_SIZE];
static int count = 0;
static int head  = 0;  // next write slot

static uint32_t g_ms_base       = 0;     // added to millis() → a timeline that survives a reload
static uint32_t g_newest_epoch  = 0;     // wall-clock (Unix s) of the newest sample; 0 if clock off
static uint32_t g_loaded_epoch  = 0;     // epoch of the newest sample restored from flash (0 = none)
static bool     g_stale_pending = false; // restored data awaiting a staleness check on first live epoch
static bool     g_frozen        = false; // sim demo: ignore live adds so the synthetic history stays put
static int      g_since_save    = 0;

#define HIST_STALE_MAX_SEC (26UL * 3600UL)  // drop restored data older than this (just past the 24h view)
#define HIST_SAVE_EVERY    10               // persist every N samples (~10 min) — easy on flash
#define HIST_MAGIC         0xC1A0
#define HIST_VERSION       1

static void usage_history_save(void);   // fwd

static inline int oldest_idx(void) {
    return (head + USAGE_HIST_SIZE - count) % USAGE_HIST_SIZE;
}

void usage_history_add(float session_pct, float weekly_pct, uint32_t epoch_now) {
    if (g_frozen) return;   // sim demo active — keep the synthetic history on screen

    // First valid clock after a reload: judge whether the restored data is stale.
    if (g_stale_pending && epoch_now != 0) {
        g_stale_pending = false;
        if (g_loaded_epoch != 0 && epoch_now > g_loaded_epoch + HIST_STALE_MAX_SEC) {
            count = 0; head = 0; g_ms_base = 0;   // too old to show as "recent" — start fresh
        }
    }

    ring[head].ms      = millis() + g_ms_base;
    ring[head].session = session_pct;
    ring[head].weekly  = weekly_pct;
    head = (head + 1) % USAGE_HIST_SIZE;
    if (count < USAGE_HIST_SIZE) count++;
    if (epoch_now != 0) g_newest_epoch = epoch_now;

    if (++g_since_save >= HIST_SAVE_EVERY) { g_since_save = 0; usage_history_save(); }
}

void usage_history_set_frozen(bool frozen) { g_frozen = frozen; }

// Clear the in-RAM history (e.g. on leaving sim mode, so the synthetic demo data
// is never persisted as if it were real). The flash copy is untouched.
void usage_history_reset(void) {
    count = 0; head = 0;
    g_ms_base = 0; g_newest_epoch = 0; g_loaded_epoch = 0;
    g_stale_pending = false; g_since_save = 0;
}

// Clear RAM and wipe the persisted copy from flash.
void usage_history_clear_saved(void) {
    usage_history_reset();
    Preferences pr;
    if (pr.begin("clawdmeter", false)) { pr.remove("hist"); pr.end(); }
}

// Compact flash format: 12-byte header {magic, version, count, pad, newest_epoch}
// followed by count x {session_u8, weekly_u8}. Timestamps aren't stored — samples
// are ~60s apart, so the reload rebuilds a 60s-spaced timeline; newest_epoch is
// kept only so a restore can tell whether the data is too old to still be "recent".
static uint8_t hist_buf[12 + 2 * USAGE_HIST_SIZE];

static void usage_history_save(void) {
    // Persist regardless of clock: newest_epoch is stored (0 if the clock is off)
    // and simply skips the staleness check on reload rather than losing the data.
    int c = count;
    if (c < 2) return;
    hist_buf[0] = HIST_MAGIC & 0xFF;    hist_buf[1] = (HIST_MAGIC >> 8) & 0xFF;
    hist_buf[2] = HIST_VERSION;         hist_buf[3] = 0;
    hist_buf[4] = c & 0xFF;             hist_buf[5] = (c >> 8) & 0xFF;
    hist_buf[6] = 0;                    hist_buf[7] = 0;
    hist_buf[8]  = g_newest_epoch & 0xFF;         hist_buf[9]  = (g_newest_epoch >> 8) & 0xFF;
    hist_buf[10] = (g_newest_epoch >> 16) & 0xFF; hist_buf[11] = (g_newest_epoch >> 24) & 0xFF;
    for (int i = 0; i < c; i++) {
        const UsageHistPoint* p = usage_history_at(i);
        int s = (int)(p->session + 0.5f); if (s < 0) s = 0; if (s > 100) s = 100;
        int w = (int)(p->weekly  + 0.5f); if (w < 0) w = 0; if (w > 100) w = 100;
        hist_buf[12 + 2 * i]     = (uint8_t)s;
        hist_buf[12 + 2 * i + 1] = (uint8_t)w;
    }
    Preferences pr;
    if (pr.begin("clawdmeter", false)) {
        pr.putBytes("hist", hist_buf, (size_t)(12 + 2 * c));
        pr.end();
    }
}

void usage_history_load(void) {
    Preferences pr;
    if (!pr.begin("clawdmeter", true)) return;   // read-only
    size_t len = pr.getBytesLength("hist");
    if (len < 12 || len > sizeof(hist_buf)) { pr.end(); return; }
    pr.getBytes("hist", hist_buf, len);
    pr.end();

    uint16_t magic = hist_buf[0] | (hist_buf[1] << 8);
    if (magic != HIST_MAGIC || hist_buf[2] != HIST_VERSION) return;
    int c = hist_buf[4] | (hist_buf[5] << 8);
    if (c < 2 || c > USAGE_HIST_SIZE) return;
    if ((size_t)(12 + 2 * c) > len) return;   // truncated blob — ignore
    uint32_t ep = (uint32_t)hist_buf[8] | ((uint32_t)hist_buf[9] << 8)
                | ((uint32_t)hist_buf[10] << 16) | ((uint32_t)hist_buf[11] << 24);

    // Rebuild a 60s-spaced timeline ending at a large base so newest-oldest
    // differences never underflow, and later live adds continue past it.
    const uint32_t step = 60000UL;
    const uint32_t base = (uint32_t)(USAGE_HIST_SIZE + 4) * step;
    for (int i = 0; i < c; i++) {
        uint32_t age = (uint32_t)(c - 1 - i);
        ring[i].ms      = base - age * step;
        ring[i].session = (float)hist_buf[12 + 2 * i];
        ring[i].weekly  = (float)hist_buf[12 + 2 * i + 1];
    }
    count = c;
    head  = c % USAGE_HIST_SIZE;
    g_ms_base = base;
    g_newest_epoch = ep;
    g_loaded_epoch = ep;
    g_stale_pending = (ep != 0);   // validate against wall-clock on the first live update
}

int usage_history_count(void) {
    return count;
}

// Cheap deterministic hash noise in [-1, 1) — reproducible so the sim looks the
// same every run (no dependence on Arduino random() state).
static float sim_noise(int i) {
    uint32_t x = (uint32_t)i * 2654435761u;
    x ^= x >> 13; x *= 1274126177u; x ^= x >> 16;
    return (float)(x & 0xFFFF) / 32768.0f - 1.0f;
}

void usage_history_fill_sim(void) {
    // Fill the ring completely: oldest at index 0, newest at SIZE-1 (see
    // oldest_idx() with count==SIZE, head==0). Timestamps step back 60s from a
    // large synthetic "now" so newest-oldest differences never underflow uint32.
    count = USAGE_HIST_SIZE;
    head  = 0;
    const uint32_t step = 60000UL;
    const uint32_t base = (uint32_t)(USAGE_HIST_SIZE + 4) * step;

    for (int i = 0; i < USAGE_HIST_SIZE; i++) {
        uint32_t age = (uint32_t)(USAGE_HIST_SIZE - 1 - i);   // 60s samples before newest
        ring[i].ms = base - age * step;
        float t = (float)i;

        // Session (5h cadence): a ~300-sample sawtooth 6->78% that resets like the
        // real 5h window, plus a mid oscillation and fine jitter. Zoom in → jitter;
        // zoom out → the sawtooth resets dominate.
        float phase = fmodf(t, 300.0f) / 300.0f;
        float sess = 6.0f + 72.0f * phase
                   + 9.0f * sinf(t / 37.0f)
                   + 4.0f * sinf(t / 6.3f)
                   + 2.5f * sim_noise(i);
        if (sess < 0) sess = 0;  if (sess > 100) sess = 100;
        ring[i].session = sess;

        // Weekly (7d cadence): a slow 24h ramp 24->66% with a gentle wave — nearly
        // flat on short zooms, clearly rising across the 24h window.
        float wk = 24.0f + 42.0f * (t / (float)USAGE_HIST_SIZE)
                 + 3.0f * sinf(t / 210.0f)
                 + 1.2f * sim_noise(i * 7 + 3);
        if (wk < 0) wk = 0;  if (wk > 100) wk = 100;
        ring[i].weekly = wk;
    }
    g_ms_base       = base;   // keep the timeline consistent if live adds resume after 'simoff'
    g_newest_epoch  = 0;
    g_stale_pending = false;
    g_frozen        = true;   // freeze live adds so the demo history stays on screen
}

const UsageHistPoint* usage_history_at(int i) {
    if (i < 0 || i >= count) return nullptr;
    return &ring[(oldest_idx() + i) % USAGE_HIST_SIZE];
}

// Shared slope helper. `weekly` selects the series. Walks oldest→newest, moving
// the window start forward whenever a reset drop is seen, then fits a simple
// endpoint slope over the surviving span.
static bool rate_of(bool weekly, float* out_rate) {
    if (count < 2) return false;

    int start = 0;
    float prev = weekly ? usage_history_at(0)->weekly : usage_history_at(0)->session;
    for (int i = 1; i < count; i++) {
        const UsageHistPoint* p = usage_history_at(i);
        float v = weekly ? p->weekly : p->session;
        if (v + RESET_DROP_PP < prev) start = i;  // reset — restart the window here
        prev = v;
    }

    const UsageHistPoint* ps = usage_history_at(start);
    const UsageHistPoint* pn = usage_history_at(count - 1);
    uint32_t dt = pn->ms - ps->ms;
    if (dt < HIST_MIN_WINDOW_MS) return false;

    float dp = (weekly ? pn->weekly : pn->session) - (weekly ? ps->weekly : ps->session);
    if (dp < 0.0f) dp = 0.0f;
    *out_rate = dp * 3600000.0f / (float)dt;   // %/hour
    return true;
}

bool usage_history_session_rate(float* pct_per_hour) { return rate_of(false, pct_per_hour); }
bool usage_history_weekly_rate(float* pct_per_hour)  { return rate_of(true,  pct_per_hour); }
