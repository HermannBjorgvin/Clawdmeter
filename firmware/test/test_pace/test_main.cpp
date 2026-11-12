// pace: where the pace marker sits on a usage bar and what colour the fill is.
//   pio test -d firmware -e native -f test_pace
#include <unity.h>
#include "pace.h"

void setUp(void) {}
void tearDown(void) {}

static void test_elapsed_share(void) {
    // 2.5 days left of the week: 64.3 % elapsed.
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 64.3f, pace_elapsed_pct(60 * 60, PACE_WEEK_MINS));
    // 3 hours left of the 5-hour window: 40 % elapsed.
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 40.0f, pace_elapsed_pct(180, PACE_SESSION_MINS));
}

static void test_reset_edges(void) {
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, pace_elapsed_pct(-1, PACE_SESSION_MINS));    // unknown
    TEST_ASSERT_EQUAL_FLOAT(100.0f, pace_elapsed_pct(0, PACE_SESSION_MINS));    // run out
    TEST_ASSERT_EQUAL_FLOAT(0.0f, pace_elapsed_pct(400, PACE_SESSION_MINS));    // skew past the window
}

static void test_levels(void) {
    TEST_ASSERT_EQUAL(PACE_UNKNOWN, pace_level(50.0f, -1.0f));
    TEST_ASSERT_EQUAL(PACE_ON, pace_level(30.0f, 64.3f));
    TEST_ASSERT_EQUAL(PACE_ON, pace_level(65.0f, 64.3f));     // touching the marker
    TEST_ASSERT_EQUAL(PACE_AHEAD, pace_level(70.0f, 64.3f));
    TEST_ASSERT_EQUAL(PACE_LIMIT, pace_level(100.0f, 100.0f));
    TEST_ASSERT_EQUAL(PACE_ON, pace_level(99.0f, 100.0f));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_elapsed_share);
    RUN_TEST(test_reset_edges);
    RUN_TEST(test_levels);
    return UNITY_END();
}
