#pragma once
#include <stdio.h>

// Pure Enterprise spend forecast for the Daily card. Free of LVGL/Arduino so it
// can be unit-tested on the host (see test/test_spend_forecast/).

struct SpendForecast {
    long daily_minor;    // average spend per day so far, in minor units
    int  projected_pct;  // projected end-of-period spend as % of the limit
    long runout_mins;    // minutes until the limit at this rate; 0 = reached, -1 = lasts past reset
};

// Elapsed time comes from the period length and minutes to reset, both already
// in the payload. The first day counts as a whole day so a few minutes of use
// is not extrapolated into a huge daily rate.
static inline SpendForecast spend_forecast(long used, long limit, int period_days, long mins_to_reset) {
    SpendForecast f = {0, 0, -1};
    if (used < 0 || limit <= 0 || period_days <= 0) return f;
    const long period_mins = (long)period_days * 1440;
    long left = mins_to_reset < 0 ? 0 : mins_to_reset > period_mins ? period_mins : mins_to_reset;
    long elapsed = period_mins - left;
    if (elapsed < 1440) elapsed = 1440;

    f.daily_minor = (long)((double)used * 1440.0 / elapsed);
    const double projected = used + (double)used * left / elapsed;
    f.projected_pct = (int)(projected * 100.0 / limit + 0.5);
    if (used >= limit) { f.runout_mins = 0; return f; }
    if (used == 0) return f;
    const double mins = (double)(limit - used) * elapsed / used;
    if (mins < left) f.runout_mins = (long)mins;
    return f;
}

// "Oct 15" for a local wall-clock epoch (seconds; the daemon's "t" field).
static inline void forecast_date(long long local_epoch, char* out, size_t n) {
    static const char* const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    // Days-to-civil from Howard Hinnant's date algorithms.
    long long z = (local_epoch >= 0 ? local_epoch : local_epoch - 86399) / 86400 + 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    snprintf(out, n, "%s %u", months[m - 1], d);
}

// "14d" / "9h" / "<1h" when no wall clock is available for a date.
static inline void forecast_eta(long mins, char* out, size_t n) {
    if (mins >= 1440)    snprintf(out, n, "%ldd", mins / 1440);
    else if (mins >= 60) snprintf(out, n, "%ldh", mins / 60);
    else                 snprintf(out, n, "<1h");
}
