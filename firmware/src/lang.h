#pragma once
#include "data.h"   // ATTN_STYLED_COUNT

// UI string table — every user-visible phrase in one place, switched at
// runtime by the daemon's "lang" payload field (host config: `lang = ru|en`).
// The choice persists in NVS so the pairing screen speaks the right language
// from the next boot on; until any payload says otherwise a fresh device
// defaults to English (upstream behaviour).

struct Strings {
    const char* title;                    // usage-view header ("Usage")
    const char* pill_session;
    const char* pill_weekly;
    const char* pill_period;              // Enterprise second panel
    const char* spending;                 // Enterprise first panel pill
    const char* of_monthly_budget;
    const char* pace_under, *pace_on, *pace_over;
    // printf formats: countdown + "%s" tail for the optional " (21:00)" part
    const char* reset_m, *reset_hm, *reset_dh;
    const char* ent_reset_fmt;            // recolor fmt: pace hex, pace text, text hex, reset date
    const char* pair1, *pair2, *pair3;    // pairing hint lines
    const char* st_waiting, *st_no_data, *st_listening, *st_connected, *st_resting;
    const char* err_auth, *err_token, *err_rate, *err_net, *err_api;
    const char* attn_caption[ATTN_STYLED_COUNT];   // indexed by ATTN_* - 1
    const char* attn_status[ATTN_STYLED_COUNT];
    const char* const* words;             // animated status-line verbs
    int word_count;
};

extern const Strings* S;

// Load the persisted language (call once, before ui_init()).
void strings_init(void);
// "ru" → Russian, "en" → English, "" / unknown → no change. Persists on
// change. Returns true when the active table actually switched — the caller
// should then restamp any labels that aren't rewritten every update.
bool strings_set_lang(const char* lang);
