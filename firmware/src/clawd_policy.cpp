#include "clawd_policy.h"
#include <string.h>

// Pure: no Arduino, no LVGL. See clawd_policy.h.

static const uint32_t SEC = 1000UL;
static const uint32_t MIN = 60UL * SEC;

void clawd_tuning_defaults(ClawdTuning* t) {
    t->show_after_ms    = 5 * MIN;
    t->show_cooldown_ms = 15 * MIN;
    t->show_scene_ms    = 16 * SEC;    // + walk-in / outro ≈ the ~20 s show
    t->quiet_ms         = 10 * MIN;
    t->calm_min_ms      = 3 * MIN;
    t->calm_max_ms      = 5 * MIN;
    t->calm_scene_ms    = 10 * SEC;
    t->alert_timeout_ms = 2 * MIN;
    t->long_turn_ms     = 2 * MIN;
    t->joy_pending_ms   = 5 * MIN;
    t->react_pending_ms = 15 * SEC;   // the walk back from a lurk is ~6 s at 2.5x
    t->blink_min_ms     = 2 * SEC;
    t->blink_max_ms     = 6 * SEC;
    t->peek_look_min_ms = 1600;       // then it rests, curious
    t->peek_look_max_ms = 2400;
    t->peek_curious_ms  = 5 * MIN;
    t->peek_double_pct  = 30;
    t->peek_toward_pct  = 60;
}

void clawd_tuning_fast(ClawdTuning* t) {
    clawd_tuning_defaults(t);
    t->show_after_ms    = 8 * SEC;
    t->show_cooldown_ms = 60 * SEC;
    t->show_scene_ms    = 6 * SEC;
    t->quiet_ms         = 15 * SEC;
    t->calm_min_ms      = 8 * SEC;
    t->calm_max_ms      = 10 * SEC;
    t->calm_scene_ms    = 5 * SEC;
    t->alert_timeout_ms = 20 * SEC;
    t->long_turn_ms     = 10 * SEC;
    t->joy_pending_ms   = 30 * SEC;
    t->peek_curious_ms  = 12 * SEC;   // under quiet_ms, as the defaults are
}

ClawdTuning& clawd_tuning(void) {
    static ClawdTuning t;
    static bool init = false;
    if (!init) { clawd_tuning_defaults(&t); init = true; }
    return t;
}

// ─── Randomness ──────────────────────────────────────────────────────────────
void clawd_rng_seed(ClawdRng* r, uint32_t seed) { r->s = seed ? seed : 0x9E3779B9u; }

uint32_t clawd_rng_next(ClawdRng* r) {
    uint32_t x = r->s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->s = x;
    return x;
}

uint32_t clawd_rng_range(ClawdRng* r, uint32_t lo, uint32_t hi) {
    if (hi <= lo) return lo;
    return lo + clawd_rng_next(r) % (hi - lo + 1);
}

bool clawd_rng_chance(ClawdRng* r, uint8_t pct) {
    return clawd_rng_next(r) % 100 < pct;
}

int clawd_pick_no_repeat(const char* const* names, int count, const char* last, ClawdRng* r) {
    int cand[16];
    int n = 0, any = -1;
    for (int i = 0; i < count && n < 16; i++) {
        if (!names[i]) continue;
        any = i;
        if (last && strcmp(names[i], last) == 0) continue;
        cand[n++] = i;
    }
    if (n == 0) return any;            // only `last` (or nothing) available
    return cand[clawd_rng_next(r) % n];
}

uint32_t clawd_jitter(uint32_t base_ms, ClawdRng* r) {
    const uint32_t lo = base_ms * 6 / 10, hi = base_ms * 14 / 10;
    return clawd_rng_range(r, lo, hi);
}

// ─── Show gate ───────────────────────────────────────────────────────────────
void show_gate_clear(ShowGate* g) { memset(g, 0, sizeof(*g)); }

void show_gate_update(ShowGate* g, uint32_t now, int activity) {
    if (activity >= 3) {
        if (!g->busy) { g->busy = true; g->busy_since = now; }
    } else {
        g->busy = false;
    }
}

bool show_gate_ready(const ShowGate* g, uint32_t now, const ClawdTuning* t) {
    if (!g->busy) return false;
    if ((uint32_t)(now - g->busy_since) < t->show_after_ms) return false;
    if (g->shown && (uint32_t)(now - g->last_show) < t->show_cooldown_ms) return false;
    return true;
}

void show_gate_mark_shown(ShowGate* g, uint32_t now) {
    g->shown = true;
    g->last_show = now;
    if (g->busy) g->busy_since = now;
}

// ─── Screensaver gate ────────────────────────────────────────────────────────
bool screensaver_ready(const QuietInput* in, const ClawdTuning* t) {
    if (!in->meters_live) return false;
    if (!in->hooks_seen && !in->window_not_started) return false;
    return (uint32_t)(in->now - in->last_engagement) >= t->quiet_ms;
}

// ─── Turn tracker ────────────────────────────────────────────────────────────
void turn_clear(TurnTracker* t) { memset(t, 0, sizeof(*t)); }

bool turn_on_event(TurnTracker* t, char event, uint32_t now, const ClawdTuning* tune) {
    if (event == 'p' || event == 'b') {
        // A follow-up prompt mid-turn keeps the original start: the turn is
        // the whole stretch of Claude working, not the last message.
        if (!t->in_turn) { t->in_turn = true; t->prompt_at = now; }
        return false;
    }
    if (event == 's') {
        const bool long_turn = t->in_turn &&
            (uint32_t)(now - t->prompt_at) >= tune->long_turn_ms;
        t->in_turn = false;
        return long_turn;
    }
    return false;
}

// ─── Shows ───────────────────────────────────────────────────────────────────
int clawd_show_group(int base_group) {
    if (base_group > 3) base_group = 3;
    return base_group < 2 ? 2 : base_group;      // RATE_ACTIVE floor
}

// ─── Attention ───────────────────────────────────────────────────────────────
bool attention_resumes_screensaver(AttnFrom from, AttnEnd how, bool engaged) {
    if (from != ATTN_FROM_SCREENSAVER) return false;
    if (how == ATTN_END_TAP) return false;
    return !engaged;
}

bool attention_end_tells_host(AttnEnd how) {
    return how != ATTN_END_CLEAR;
}

bool reaction_still_due(uint32_t queued_at, uint32_t now, const ClawdTuning* t) {
    return (uint32_t)(now - queued_at) <= t->react_pending_ms;
}

// ─── A tap on the meters ─────────────────────────────────────────────────────
TapReact clawd_tap_react(const TapInput* in) {
    if (in->minimal || !in->on_meters) return TAP_NONE;
    return in->corner == CORNER_FREE ? TAP_PEEK_HOP : TAP_PEEK;
}

PeekHold peek_hold(uint32_t up_since, uint32_t now, const ClawdTuning* t) {
    return (uint32_t)(now - up_since) >= t->peek_curious_ms ? PEEK_HOLD_DONE : PEEK_HOLD_KEEP;
}

int peek_glance_px(int tap_x, int width, int max_px) {
    const int half = width / 2;
    if (half <= 0) return 0;
    int g = (tap_x - half) * max_px / half;
    if (g > max_px)  g = max_px;
    if (g < -max_px) g = -max_px;
    return g;
}

// ─── Corner seat ─────────────────────────────────────────────────────────────
int corner_seat_feet(int feet_y, int tallest_px, int max_feet, int top_margin) {
    const int need = tallest_px + top_margin;
    if (feet_y >= need) return feet_y;
    return need <= max_feet ? need : (max_feet > feet_y ? max_feet : feet_y);
}

// ─── Resting face ────────────────────────────────────────────────────────────
FacePlan face_plan_next(ClawdRng* r, const ClawdTuning* t) {
    FacePlan p;
    p.gap_ms = clawd_rng_range(r, t->blink_min_ms, t->blink_max_ms);
    const uint32_t roll = clawd_rng_next(r) % 100;
    if (roll < 45)       p.act = FACE_BLINK;
    else if (roll < 57)  p.act = FACE_DOUBLE_BLINK;     // ~1 in 5 blinks is double
    else if (roll < 79)  p.act = FACE_GLANCE_LEFT;
    else                 p.act = FACE_GLANCE_RIGHT;
    return p;
}

int face_glance_px(FaceAct act, int glance_px, int tap_g, ClawdRng* r, const ClawdTuning* t) {
    if (tap_g != 0 && clawd_rng_chance(r, t->peek_toward_pct)) return tap_g;
    return act == FACE_GLANCE_LEFT ? -glance_px : glance_px;
}
