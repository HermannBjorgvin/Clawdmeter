// usage_rate: 15-minute window, %/h share-of-pace groups, replay skipping,
// hook activity lift, reset detection.
//   pio test -d firmware -e native -f test_usage_rate
#include <unity.h>
#include "usage_rate.h"

static const uint32_t MIN = 60UL * 1000UL;
static const uint32_t T0  = 1000;   // arbitrary boot offset

void setUp(void) { usage_rate_clear(); }
void tearDown(void) {}

// Feed one real sample per minute from `from` to `to` (inclusive) for a
// linear climb, starting at T0.
static void climb(float from, float to, int minutes) {
    for (int m = 0; m <= minutes; m++) {
        float pct = from + (to - from) * m / (float)minutes;
        usage_rate_sample_at(T0 + m * MIN, (float)(int)pct, false);   // whole points, like the API
    }
}

static void test_no_samples_is_idle(void) {
    TEST_ASSERT_EQUAL_INT(RATE_IDLE, usage_rate_group_at(T0));
    usage_rate_sample_at(T0, 40, false);
    TEST_ASSERT_EQUAL_INT(RATE_IDLE, usage_rate_group_at(T0 + MIN));
}

static void test_one_point_in_window_is_idle(void) {
    climb(10, 11, 15);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 4.0f, usage_rate_pct_per_hour_at(T0 + 15 * MIN));
    TEST_ASSERT_EQUAL_INT(RATE_IDLE, usage_rate_group_at(T0 + 15 * MIN));
}

static void test_two_points_is_normal(void) {
    climb(10, 12, 15);
    TEST_ASSERT_EQUAL_INT(RATE_NORMAL, usage_rate_group_at(T0 + 15 * MIN));
}

static void test_three_points_is_active(void) {
    climb(10, 13, 15);
    TEST_ASSERT_EQUAL_INT(RATE_ACTIVE, usage_rate_group_at(T0 + 15 * MIN));
}

static void test_pace_is_heavy(void) {
    climb(10, 15, 15);    // 5 points / 15 min = 20 %/h = exactly pace
    TEST_ASSERT_EQUAL_INT(RATE_HEAVY, usage_rate_group_at(T0 + 15 * MIN));
}

static void test_short_history_divides_by_full_window(void) {
    // 1 point a minute after boot must not read as 60 %/h.
    usage_rate_sample_at(T0, 10, false);
    usage_rate_sample_at(T0 + MIN, 11, false);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 4.0f, usage_rate_pct_per_hour_at(T0 + MIN));
    TEST_ASSERT_EQUAL_INT(RATE_IDLE, usage_rate_group_at(T0 + MIN));
}

static void test_decays_to_idle_without_new_samples(void) {
    climb(10, 15, 15);
    TEST_ASSERT_EQUAL_INT(RATE_HEAVY, usage_rate_group_at(T0 + 15 * MIN));
    // Nothing new for a full window: the baseline becomes the newest sample.
    TEST_ASSERT_EQUAL_INT(RATE_IDLE, usage_rate_group_at(T0 + 30 * MIN));
}

static void test_replays_are_ignored(void) {
    usage_rate_sample_at(T0, 10, false);
    // A replay with a wild value moves nothing and is never a reset.
    TEST_ASSERT_FALSE(usage_rate_sample_at(T0 + 5 * MIN, 90, true));
    TEST_ASSERT_FALSE(usage_rate_sample_at(T0 + 6 * MIN, 0, true));
    TEST_ASSERT_EQUAL_INT(RATE_IDLE, usage_rate_group_at(T0 + 6 * MIN));
    // Heartbeat replays of the same value don't dilute a real climb either.
    for (int m = 1; m <= 15; m++) usage_rate_sample_at(T0 + m * MIN + 30000, 10, true);
    usage_rate_sample_at(T0 + 15 * MIN, 15, false);
    TEST_ASSERT_EQUAL_INT(RATE_HEAVY, usage_rate_group_at(T0 + 15 * MIN));
}

static void test_reset_detection(void) {
    TEST_ASSERT_FALSE(usage_rate_sample_at(T0, 80, false));          // first sample: never
    TEST_ASSERT_FALSE(usage_rate_sample_at(T0 + MIN, 76, false));    // -4: jitter, not a reset
    TEST_ASSERT_TRUE(usage_rate_sample_at(T0 + 2 * MIN, 2, false));  // big drop
    // Tracking restarted from the reset sample, so no phantom rate.
    TEST_ASSERT_EQUAL_INT(RATE_IDLE, usage_rate_group_at(T0 + 2 * MIN));
    TEST_ASSERT_FALSE(usage_rate_sample_at(T0 + 3 * MIN, 3, false));
}

static void test_reset_ignored_on_replay(void) {
    usage_rate_sample_at(T0, 80, false);
    TEST_ASSERT_FALSE(usage_rate_sample_at(T0 + MIN, 0, true));
    TEST_ASSERT_TRUE(usage_rate_sample_at(T0 + 2 * MIN, 0, false));
}

static void test_activity_lifts_never_lowers(void) {
    usage_rate_set_activity_at(T0, 2);
    TEST_ASSERT_EQUAL_INT(RATE_ACTIVE, usage_rate_group_at(T0));     // idle rate, lifted
    climb(10, 16, 15);                                               // heavy rate
    usage_rate_set_activity_at(T0 + 15 * MIN, 1);
    TEST_ASSERT_EQUAL_INT(RATE_HEAVY, usage_rate_group_at(T0 + 15 * MIN));
    usage_rate_set_activity_at(T0 + 15 * MIN, 0);
    TEST_ASSERT_EQUAL_INT(RATE_HEAVY, usage_rate_group_at(T0 + 15 * MIN));
}

static void test_activity_expires(void) {
    usage_rate_set_activity_at(T0, 3);
    TEST_ASSERT_EQUAL_INT(3, usage_rate_activity_at(T0 + USAGE_RATE_ACTIVITY_TTL_MS - 1));
    TEST_ASSERT_EQUAL_INT(-1, usage_rate_activity_at(T0 + USAGE_RATE_ACTIVITY_TTL_MS));
    TEST_ASSERT_EQUAL_INT(RATE_IDLE, usage_rate_group_at(T0 + USAGE_RATE_ACTIVITY_TTL_MS));
    usage_rate_set_activity_at(T0, 3);
    usage_rate_set_activity_at(T0, -1);                               // explicit clear
    TEST_ASSERT_EQUAL_INT(-1, usage_rate_activity_at(T0));
}

static void test_close_samples_merge(void) {
    // Bursts (prompt poll + stop poll 10 s apart) don't eat ring slots:
    // 200 samples 10 s apart still leave the 15-min baseline available.
    for (int i = 0; i < 200; i++) usage_rate_sample_at(T0 + i * 10000, 10.0f + i / 20, false);
    const uint32_t now = T0 + 199 * 10000;
    // ~33 min of climb at 3 points per 10 min = 18 %/h → Active.
    TEST_ASSERT_EQUAL_INT(RATE_ACTIVE, usage_rate_group_at(now));
}

static void test_millis_wraparound(void) {
    const uint32_t start = 0xFFFFFFFFUL - 5 * MIN;
    for (int m = 0; m <= 15; m++) usage_rate_sample_at(start + m * MIN, 10.0f + m / 3, false);
    TEST_ASSERT_EQUAL_INT(RATE_HEAVY, usage_rate_group_at(start + 15 * MIN));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_no_samples_is_idle);
    RUN_TEST(test_one_point_in_window_is_idle);
    RUN_TEST(test_two_points_is_normal);
    RUN_TEST(test_three_points_is_active);
    RUN_TEST(test_pace_is_heavy);
    RUN_TEST(test_short_history_divides_by_full_window);
    RUN_TEST(test_decays_to_idle_without_new_samples);
    RUN_TEST(test_replays_are_ignored);
    RUN_TEST(test_reset_detection);
    RUN_TEST(test_reset_ignored_on_replay);
    RUN_TEST(test_activity_lifts_never_lowers);
    RUN_TEST(test_activity_expires);
    RUN_TEST(test_close_samples_merge);
    RUN_TEST(test_millis_wraparound);
    return UNITY_END();
}
