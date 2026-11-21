#include "signals.h"
#include "usage_rate.h"
#include <string.h>

// Pure: no Arduino. See signals.h.

static bool     hooks_seen = false;
static bool     have_seq   = false;
static uint32_t last_seq   = 0;
static char     last_event = '\0';
static int      sessions   = -1;
static int      meter_kind = -1;   // usage_meter_kind of the last real sample
static char     sample_account[sizeof(((UsageData*)0)->account)] = "";  // `an` of the last real sample
static bool     have_sample_account = false;                            // ... once there's been one
static char     shown_account[sizeof(((UsageData*)0)->account)] = "";   // `an` of the last payload with one
static bool     shown_auto = false;  // its `al`

void signals_clear(void) {
    hooks_seen = false;
    have_seq = false;
    last_seq = 0;
    last_event = '\0';
    sessions = -1;
    meter_kind = -1;
    sample_account[0] = '\0';
    have_sample_account = false;
    shown_account[0] = '\0';
    shown_auto = false;
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
    // Codex numbers aren't Claude's: Clawd's slow clock, his reaction to a
    // rising meter and the reset chime follow Claude only, so to them a
    // Codex payload's numbers are a replay. Its hook fields still count.
    if (d->codex) u.replay = true;
    const bool real_sample = d->ok && !d->replay && !d->codex;
    // The main meter changed what it measures (session, week or spend: an
    // account verified for no 5-hour limit, or a reading that couldn't tell
    // after an app restart). Its numbers aren't comparable: start over
    // rather than read the jump as a reset.
    if (real_sample) {
        const int kind = usage_meter_kind(d);
        if (meter_kind >= 0 && kind != meter_kind) {
            usage_rate_restart();
            u.meter_kind_changed = true;
        }
        meter_kind = kind;
        // Another account's numbers (`an`, the app picked another one):
        // not comparable either, so a drop isn't a reset and a jump isn't
        // a rise. No name counts as a name: the app names accounts only
        // with two or more, so adding or removing one (and switching on
        // the way) moves between a name and none. Codex payloads never get
        // here, so Claude A, Codex, then Claude A again keeps A's history.
        // An older app never names one: nothing changes for it.
        if (have_sample_account && strcmp(d->account, sample_account) != 0) {
            usage_rate_restart();
            u.meter_kind_changed = true;
        }
        memcpy(sample_account, d->account, sizeof(sample_account));
        have_sample_account = true;
    }
    // The account on the device changed, or how it was picked (`al`: the
    // latest active one or a chosen one), in any payload naming one,
    // replays and Codex included: the UI names it for a moment. The first
    // name since boot is a baseline.
    if (d->account[0]) {
        u.account_switched = shown_account[0]
                          && (strcmp(d->account, shown_account) != 0 || d->account_auto != shown_auto);
        memcpy(shown_account, d->account, sizeof(shown_account));
        shown_auto = d->account_auto;
    }
    u.session_reset = usage_rate_sample_at(now_ms, d->session_pct, !real_sample);

    if (d->has_event_seq) {
        hooks_seen = true;
        if (!have_seq) {
            have_seq = true;                 // baseline, don't react
            last_seq = d->event_seq;
        } else if (d->event_seq != last_seq) {
            // Fires on a replay too: `rp` only says the usage numbers are
            // repeated. The app sends a fresh event in an `rp:1` write ahead
            // of the poll, and `es` alone keeps it from firing twice.
            last_seq = d->event_seq;
            u.event_fired = true;
            u.event = d->event;
            u.event_seq = d->event_seq;
            last_event = d->event;
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
