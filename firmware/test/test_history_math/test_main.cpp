// Host unit test for history_math.h — the arithmetic behind the History screen.
//
//   c++ -std=c++17 -I ../../src test_main.cpp -o t && ./t

#include "history_math.h"
#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static void week_cumulative_starts_on_monday() {
    // 14 days ending on a Wednesday (weekday 2): the week is Mon, Tue, Wed.
    int16_t out[HIST_DAYS] = {9,9,9,9,9,9,9,9,9,9,9, 100, 200, 300};
    int32_t cum[HIST_DAYS];
    int len = history_week_cumulative(out, HIST_DAYS, history_week_start(HIST_DAYS, 2, -1), cum);
    CHECK(len == 3);
    CHECK(cum[10] == -1);
    CHECK(cum[11] == 100);
    CHECK(cum[12] == 300);
    CHECK(cum[13] == 600);
}

static void week_cumulative_on_sunday_spans_seven() {
    int16_t out[HIST_DAYS] = {1,1,1,1,1,1,1, 1,1,1,1,1,1,1};
    int32_t cum[HIST_DAYS];
    CHECK(history_week_cumulative(out, HIST_DAYS, history_week_start(HIST_DAYS, 6, -1), cum) == 7);
    CHECK(cum[6] == -1);
    CHECK(cum[7] == 1);
    CHECK(cum[13] == 7);
}

static void week_cumulative_clamps_bad_weekday_and_short_history() {
    int16_t out[3] = {5, 6, 7};
    int32_t cum[3];
    CHECK(history_week_cumulative(out, 3, history_week_start(3, 99, -1), cum) == 3);   // Sun, only 3 days
    CHECK(cum[0] == 5 && cum[2] == 18);
    CHECK(history_week_cumulative(out, 0, 0, cum) == 0);
}

static void week_start_prefers_rolling_window() {
    // Sunday, but the 7-day limit window opened on the bucket at index 9.
    CHECK(history_week_start(HIST_DAYS, 6, 9) == 9);
    // Out-of-range window index → calendar fallback.
    CHECK(history_week_start(HIST_DAYS, 6, 14) == 7);
    CHECK(history_week_start(HIST_DAYS, 6, -1) == 7);
    // Window opened today.
    CHECK(history_week_start(HIST_DAYS, 6, 13) == 13);
}

static void week_cumulative_from_window_start() {
    int16_t out[HIST_DAYS] = {9,9,9,9,9,9,9,9,9, 10, 20, 30, 40, 50};
    int32_t cum[HIST_DAYS];
    CHECK(history_week_cumulative(out, HIST_DAYS, 9, cum) == 5);
    CHECK(cum[8] == -1);
    CHECK(cum[9] == 10);
    CHECK(cum[13] == 150);
}

static void today_vs_average_uses_prior_seven() {
    int16_t out[HIST_DAYS] = {1000,1000,1000,1000,1000,1000, 10,20,30,40,50,60,70, 500};
    int32_t today, avg;
    history_today_vs_avg(out, HIST_DAYS, &today, &avg);
    CHECK(today == 500);
    CHECK(avg == 40);                          // (10..70)/7
}

static void today_vs_average_with_one_day() {
    int16_t out[1] = {42};
    int32_t today, avg;
    history_today_vs_avg(out, 1, &today, &avg);
    CHECK(today == 42 && avg == 0);
}

static void max_never_below_one() {
    int16_t z[3] = {0, 0, 0};
    CHECK(history_max(z, 3) == 1);
    int16_t v[3] = {3, 9, 4};
    CHECK(history_max(v, 3) == 9);
}

static void format_tokens() {
    char b[8];
    history_fmt_k(1338, b, sizeof b); CHECK(strcmp(b, "1.3M") == 0);
    history_fmt_k(1000, b, sizeof b); CHECK(strcmp(b, "1.0M") == 0);
    history_fmt_k(881,  b, sizeof b); CHECK(strcmp(b, "881k") == 0);
    history_fmt_k(12,   b, sizeof b); CHECK(strcmp(b, "12k") == 0);
    history_fmt_k(0,    b, sizeof b); CHECK(strcmp(b, "0") == 0);
}

static void pace_unknown_while_warming_up() {
    PaceProjection p = history_pace(40.0f, 120, -1.0f);
    CHECK(p.kind == PACE_UNKNOWN);
    p = history_pace(40.0f, -1, 0.5f);
    CHECK(p.kind == PACE_UNKNOWN);
}

static void pace_idle_at_negligible_rate() {
    CHECK(history_pace(40.0f, 120, 0.0f).kind == PACE_IDLE);
    CHECK(history_pace(40.0f, 120, 0.01f).kind == PACE_IDLE);
}

static void pace_limit_before_reset() {
    // 40% used, 0.5 %/min → 120 min to 100%; resets in 180 → limit first.
    PaceProjection p = history_pace(40.0f, 180, 0.5f);
    CHECK(p.kind == PACE_LIMIT);
    CHECK(p.minutes == 120);
}

static void pace_ok_projects_pct_at_reset() {
    // 40% used, 0.2 %/min, resets in 100 → 60% at reset.
    PaceProjection p = history_pace(40.0f, 100, 0.2f);
    CHECK(p.kind == PACE_OK);
    CHECK(p.pct == 60);
}

static void pace_boundary_counts_as_limit() {
    // exactly reaches 100% at reset
    PaceProjection p = history_pace(50.0f, 100, 0.5f);
    CHECK(p.kind == PACE_LIMIT);
    CHECK(p.minutes == 100);
}

static void burn_time_pct_from_minutes_remaining() {
    CHECK(burn_time_pct(300, BURN_SESSION_MINS) == 0);     // just opened
    CHECK(burn_time_pct(150, BURN_SESSION_MINS) == 50);    // halfway
    CHECK(burn_time_pct(0,   BURN_SESSION_MINS) == 100);   // about to reset
    CHECK(burn_time_pct(10080, BURN_WEEK_MINS) == 0);
    CHECK(burn_time_pct(2520,  BURN_WEEK_MINS) == 75);
}

static void burn_time_pct_clamps_and_reports_unknown() {
    CHECK(burn_time_pct(-1, BURN_SESSION_MINS) == -1);     // no reset time
    CHECK(burn_time_pct(400, BURN_SESSION_MINS) == 0);     // more than a window left
    CHECK(burn_time_pct(150, 0) == -1);
}

int main() {
    burn_time_pct_from_minutes_remaining();
    burn_time_pct_clamps_and_reports_unknown();
    week_cumulative_starts_on_monday();
    week_cumulative_on_sunday_spans_seven();
    week_cumulative_clamps_bad_weekday_and_short_history();
    week_start_prefers_rolling_window();
    week_cumulative_from_window_start();
    today_vs_average_uses_prior_seven();
    today_vs_average_with_one_day();
    max_never_below_one();
    format_tokens();
    pace_unknown_while_warming_up();
    pace_idle_at_negligible_rate();
    pace_limit_before_reset();
    pace_ok_projects_pct_at_reset();
    pace_boundary_counts_as_limit();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("history_math: all checks passed\n");
    return 0;
}
