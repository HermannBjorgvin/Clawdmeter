// Host unit test for spend_forecast.h — the Enterprise Daily card. No Arduino/LVGL deps:
//
//   g++ -std=c++17 -I ../../src test_main.cpp -o t && ./t

#include "spend_forecast.h"
#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK_EQ(got, want)                                                    \
    do {                                                                       \
        long long g_ = (got), w_ = (want);                                     \
        if (g_ != w_) {                                                        \
            printf("FAIL %s:%d  %s = %lld want %lld\n", __FILE__, __LINE__,     \
                   #got, g_, w_);                                              \
            ++failures;                                                        \
        }                                                                      \
    } while (0)
#define CHECK_STR(got, want)                                                   \
    do {                                                                       \
        if (strcmp(got, want) != 0) {                                          \
            printf("FAIL %s:%d  got \"%s\" want \"%s\"\n", __FILE__, __LINE__,  \
                   got, want);                                                 \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static const long DAY = 1440;

int main() {
    // 30-day period, 10 days in, $100 of $500: $10/day, $300 projected, lasts.
    SpendForecast f = spend_forecast(10000, 50000, 30, 20 * DAY);
    CHECK_EQ(f.daily_minor, 1000);
    CHECK_EQ(f.projected_pct, 60);
    CHECK_EQ(f.runout_mins, -1);

    // 10 days in, $300 of $500: $30/day, $200 left runs out in 6.67 days.
    f = spend_forecast(30000, 50000, 30, 20 * DAY);
    CHECK_EQ(f.daily_minor, 3000);
    CHECK_EQ(f.projected_pct, 180);
    CHECK_EQ(f.runout_mins, 9600);

    // Exactly on budget: runs out at the reset, which counts as lasting.
    f = spend_forecast(25000, 50000, 30, 15 * DAY);
    CHECK_EQ(f.projected_pct, 100);
    CHECK_EQ(f.runout_mins, -1);

    // First hours of the period count as one whole day.
    f = spend_forecast(1250, 50000, 31, 31 * DAY - 120);
    CHECK_EQ(f.daily_minor, 1250);
    CHECK_EQ(f.runout_mins, -1);                  // $487.50 left lasts 39 days
    f = spend_forecast(25000, 50000, 31, 31 * DAY - 120);
    CHECK_EQ(f.daily_minor, 25000);
    CHECK_EQ(f.runout_mins, DAY);

    // Limit already reached or passed.
    f = spend_forecast(50000, 50000, 30, 5 * DAY);
    CHECK_EQ(f.runout_mins, 0);
    f = spend_forecast(60000, 50000, 30, 5 * DAY);
    CHECK_EQ(f.runout_mins, 0);

    // Nothing spent: zero rate, never runs out.
    f = spend_forecast(0, 50000, 30, 10 * DAY);
    CHECK_EQ(f.daily_minor, 0);
    CHECK_EQ(f.projected_pct, 0);
    CHECK_EQ(f.runout_mins, -1);

    // Bad input stays neutral rather than dividing by zero.
    f = spend_forecast(-1, 50000, 30, 10 * DAY);
    CHECK_EQ(f.runout_mins, -1);
    f = spend_forecast(1000, 0, 30, 10 * DAY);
    CHECK_EQ(f.daily_minor, 0);
    f = spend_forecast(1000, 50000, 0, 10 * DAY);
    CHECK_EQ(f.daily_minor, 0);

    // Reset further out than the period (clock skew) clamps to the first day.
    f = spend_forecast(1000, 50000, 30, 40 * DAY);
    CHECK_EQ(f.daily_minor, 1000);
    // Past the reset: whole period elapsed, nothing left to project.
    f = spend_forecast(30000, 50000, 30, -5);
    CHECK_EQ(f.daily_minor, 1000);
    CHECK_EQ(f.projected_pct, 60);
    CHECK_EQ(f.runout_mins, -1);

    char buf[16];
    forecast_date(0, buf, sizeof(buf));           CHECK_STR(buf, "Jan 1");
    forecast_date(1760486400, buf, sizeof(buf));  CHECK_STR(buf, "Oct 15");   // 2025-10-15 00:00
    forecast_date(1760572799, buf, sizeof(buf));  CHECK_STR(buf, "Oct 15");   // 23:59:59 same day
    forecast_date(1709164800, buf, sizeof(buf));  CHECK_STR(buf, "Feb 29");   // leap day 2024
    forecast_date(1735603200, buf, sizeof(buf));  CHECK_STR(buf, "Dec 31");   // 2024-12-31

    forecast_eta(14 * DAY + 300, buf, sizeof(buf)); CHECK_STR(buf, "14d");
    forecast_eta(DAY, buf, sizeof(buf));            CHECK_STR(buf, "1d");
    forecast_eta(9 * 60 + 5, buf, sizeof(buf));     CHECK_STR(buf, "9h");
    forecast_eta(59, buf, sizeof(buf));             CHECK_STR(buf, "<1h");

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all spend forecast checks passed\n");
    return 0;
}
