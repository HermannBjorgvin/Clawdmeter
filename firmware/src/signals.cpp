#include "signals.h"
#include "usage_rate.h"

// Pure: no Arduino. See signals.h.

static bool     hooks_seen = false;
static bool     have_seq   = false;
static uint32_t last_seq   = 0;
static char     last_event = '\0';
static bool     have_event_ms = false;
static uint32_t event_ms   = 0;
static bool     have_sample_ms = false;
static uint32_t sample_ms  = 0;
static int      sessions   = -1;

void signals_clear(void) {
    hooks_seen = false;
    have_seq = false;
    last_seq = 0;
    last_event = '\0';
    have_event_ms = false;
    event_ms = 0;
    have_sample_ms = false;
    sample_ms = 0;
    sessions = -1;
    usage_rate_clear();
}

SignalsUpdate signals_ingest_at(const UsageData* d, uint32_t now_ms) {
    SignalsUpdate u = {};
    u.replay = d->replay;
    u.group_before = usage_rate_group_at(now_ms);

    // Activity first so the group_after below already carries the lift.
    // Replays refresh it too: the daemon is alive and that is its level.
    if (d->activity >= 0) {
        hooks_seen = true;
        usage_rate_set_activity_at(now_ms, d->activity);
    }

    // A failed poll is not a zero: an {"ok":false} "no data" beat carries no
    // session % (it parses as 0), so it must not reach the rate ring or the
    // reset detector (it would read as a fresh window and fire the chime).
    const bool real_sample = d->ok && !d->replay;
    u.session_reset = usage_rate_sample_at(now_ms, d->session_pct, !real_sample);
    if (real_sample) { have_sample_ms = true; sample_ms = now_ms; }

    if (d->has_event_seq) {
        hooks_seen = true;
        if (!have_seq) {
            have_seq = true;                 // baseline, don't react
            last_seq = d->event_seq;
        } else if (d->event_seq != last_seq) {
            last_seq = d->event_seq;
            if (!d->replay) {
                u.event_fired = true;
                u.event = d->event;
                u.event_seq = d->event_seq;
                last_event = d->event;
                have_event_ms = true;
                event_ms = now_ms;
            }
        }
    }

    if (d->sessions >= 0) {
        hooks_seen = true;
        if (d->sessions != sessions) {
            sessions = d->sessions;
            u.sessions_changed = true;
        }
    }

    u.group_after = usage_rate_group_at(now_ms);
    return u;
}

bool     signals_hooks_seen(void) { return hooks_seen; }
int      signals_activity_at(uint32_t now_ms) { return usage_rate_activity_at(now_ms); }
int      signals_sessions(void) { return sessions; }
char     signals_last_event(void) { return last_event; }

uint32_t signals_ms_since_event_at(uint32_t now_ms) {
    return have_event_ms ? (uint32_t)(now_ms - event_ms) : UINT32_MAX;
}
uint32_t signals_ms_since_sample_at(uint32_t now_ms) {
    return have_sample_ms ? (uint32_t)(now_ms - sample_ms) : UINT32_MAX;
}
