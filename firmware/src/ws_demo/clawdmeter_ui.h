// Clawdmeter Usage screen for the Waveshare 3.49" landscape layout (640×172).
// Called once from lvgl_port_init() after LVGL is up and the display is
// registered. Builds a static layout (logo, title, two bars, money line,
// footer) and starts a 3-second cycle of mock values so we can sight-check
// the design without BLE / daemon connection.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Builds Usage / Sessions / Tokens / Settings tiles and the tileview that
// hosts them. Horizontal swipes navigate between tiles.
void clawdmeter_build_screens(void);

// Apply a snapshot of usage values from a parsed daemon payload. Cents
// arguments are in integer cents; -1 = unknown. Once called at least once,
// mock cycling stops.
void clawdmeter_apply_values(int sess_pct, int week_pct,
                             int sess_reset_mins, int week_reset_mins,
                             int batt_pct, int charging,
                             long day_cents, long week_cents, long month_cents,
                             long proj_day_cents,
                             long burn_rate_cents_per_hr,
                             int sessions_count);

// Per-bucket token+cost rows for the Tokens tile. Each window has 4 rows:
// Input, Output, Cache Read, Cache Write. tokens[0..3] are 64-bit token
// counts; cents[0..3] are int32 cost cents.
void clawdmeter_apply_tokens(long long tok_d[4], long cents_d[4],
                             long long tok_w[4], long cents_w[4],
                             long long tok_m[4], long cents_m[4]);

// Session entries for the Sessions tile.
enum { SESS_IDLE_S = 0, SESS_WORKING_S = 1, SESS_WAITING_S = 2 };
typedef struct {
    char proj[20];
    int  state;          // SESS_*
    char msg[64];
} clawdmeter_session_t;
void clawdmeter_apply_sessions(const clawdmeter_session_t *list, int count);

// Footer text (small, beneath the cards).
void clawdmeter_set_status(const char *status);

// Show or hide the full-screen "Claude is waiting" overlay. Passing NULL or
// empty string hides it.
void clawdmeter_set_attention(const char *msg);

// Settings tile updates.
void clawdmeter_settings_set_ble(const char *state, const char *mac);

#ifdef __cplusplus
}
#endif
