// buzzer: the beep timing for on/off buzzers (LCD-4, LCD-2.1/2.8C): the
// boot beep, the three-beep chime, and retrying a failed "off".
//   pio test -d firmware -e native -f test_buzzer
#include <unity.h>
#include "buzzer.h"

// A fake buzzer pin: records every write and can be told to fail.
static int  writes;
static bool level;            // what the pin was last set to (successfully)
static int  fail_next_off;    // this many "off" writes fail
static int  fail_next_on;     // this many "on" writes fail
static int  on_edges;         // successful off→on writes

static bool fake_set(bool on) {
    writes++;
    if (on  && fail_next_on  > 0) { fail_next_on--;  return false; }
    if (!on && fail_next_off > 0) { fail_next_off--; return false; }
    if (on && !level) on_edges++;
    level = on;
    return true;
}

void setUp(void) {
    writes = 0;
    level = false;
    fail_next_off = 0;
    fail_next_on = 0;
    on_edges = 0;
}
void tearDown(void) {}

// Ticks every millisecond from `from` to `to` (exclusive).
static void run(uint32_t from, uint32_t to) {
    for (uint32_t t = from; t < to; t++) buzzer_tick(t);
}

// Init at 0, then let the boot beep finish, so a test starts idle at 1000
// with the counters cleared.
static void init_idle(void) {
    buzzer_init(fake_set, 0);
    run(0, 1000);
    TEST_ASSERT_FALSE(buzzer_busy());
    writes = 0;
    on_edges = 0;
}

static const uint32_t PATTERN_MS =
    BUZZER_BEEPS * BUZZER_ON_MS + (BUZZER_BEEPS - 1) * BUZZER_GAP_MS;

static void test_init_switches_off_then_beeps_once(void) {
    level = true;
    buzzer_init(fake_set, 0);
    TEST_ASSERT_FALSE(level);            // off first, at once
    TEST_ASSERT_EQUAL_INT(1, writes);
    TEST_ASSERT_TRUE(buzzer_busy());
    // The boot beep plays from the first tick (after setup) for BUZZER_ON_MS.
    buzzer_tick(500);
    TEST_ASSERT_TRUE(level);
    run(501, 500 + BUZZER_ON_MS);
    TEST_ASSERT_TRUE(level);
    buzzer_tick(500 + BUZZER_ON_MS);
    TEST_ASSERT_FALSE(level);
    run(500 + BUZZER_ON_MS + 1, 3000);
    TEST_ASSERT_EQUAL_INT(BUZZER_BOOT_BEEPS, on_edges);
    TEST_ASSERT_FALSE(buzzer_busy());
}

static void test_three_beeps_with_their_timing(void) {
    init_idle();
    buzzer_start(1000, BUZZER_BEEPS);
    // Sample the pin each millisecond and time each span.
    int on_ms = 0, rises = 0;
    bool was = false;
    uint32_t last_rise = 0, first_rise = 0, last_fall = 0;
    for (uint32_t t = 1000; t < 2000; t++) {
        buzzer_tick(t);
        if (level) on_ms++;
        if (level && !was) {
            if (rises > 0) TEST_ASSERT_EQUAL_UINT32(BUZZER_ON_MS + BUZZER_GAP_MS, t - last_rise);
            else first_rise = t;
            last_rise = t;
            rises++;
        }
        if (!level && was) last_fall = t;
        was = level;
    }
    TEST_ASSERT_EQUAL_INT(BUZZER_BEEPS, rises);
    TEST_ASSERT_EQUAL_INT(BUZZER_BEEPS * BUZZER_ON_MS, on_ms);
    TEST_ASSERT_EQUAL_UINT32(1000, first_rise);              // starts at once
    TEST_ASSERT_EQUAL_UINT32(1000 + PATTERN_MS, last_fall);  // ends off
    TEST_ASSERT_FALSE(level);
    TEST_ASSERT_FALSE(buzzer_busy());
}

static void test_second_chime_while_playing_is_ignored(void) {
    init_idle();
    buzzer_start(1000, BUZZER_BEEPS);
    run(1000, 1150);
    buzzer_start(1150, BUZZER_BEEPS);   // mid-pattern: ignored, not restarted
    run(1150, 2000);
    TEST_ASSERT_EQUAL_INT(BUZZER_BEEPS, on_edges);
    // Once it has finished, a new chime plays in full.
    buzzer_start(2000, BUZZER_BEEPS);
    run(2000, 3000);
    TEST_ASSERT_EQUAL_INT(2 * BUZZER_BEEPS, on_edges);
    TEST_ASSERT_FALSE(level);
}

static void test_failed_off_stops_the_pattern_and_retries(void) {
    init_idle();
    const uint32_t t0 = 1000;
    buzzer_start(t0, BUZZER_BEEPS);
    buzzer_tick(t0);             // first beep on
    TEST_ASSERT_TRUE(level);
    fail_next_off = 2;                 // the next two "off" writes fail
    buzzer_tick(t0 + BUZZER_ON_MS);   // off fails: pattern dropped
    TEST_ASSERT_TRUE(level);
    TEST_ASSERT_TRUE(buzzer_busy());
    // Nothing more until the retry is due.
    int before = writes;
    run(t0 + BUZZER_ON_MS + 1, t0 + BUZZER_ON_MS + BUZZER_OFF_RETRY_MS);
    TEST_ASSERT_EQUAL_INT(before, writes);
    buzzer_tick(t0 + BUZZER_ON_MS + BUZZER_OFF_RETRY_MS);       // fails again
    TEST_ASSERT_TRUE(level);
    buzzer_tick(t0 + BUZZER_ON_MS + 2 * BUZZER_OFF_RETRY_MS);   // lands
    TEST_ASSERT_FALSE(level);
    TEST_ASSERT_FALSE(buzzer_busy());
    run(t0 + BUZZER_ON_MS + 2 * BUZZER_OFF_RETRY_MS + 1, 4000);
    TEST_ASSERT_EQUAL_INT(1, on_edges);   // the rest of the pattern stays dropped
}

static void test_chime_waits_for_a_pending_off(void) {
    fail_next_off = 1;
    buzzer_init(fake_set, 0);    // the boot "off" fails: no boot beep
    TEST_ASSERT_TRUE(buzzer_busy());
    buzzer_start(10, BUZZER_BEEPS);   // ignored: the off comes first
    buzzer_tick(BUZZER_OFF_RETRY_MS);
    TEST_ASSERT_FALSE(level);
    TEST_ASSERT_FALSE(buzzer_busy());
    run(BUZZER_OFF_RETRY_MS + 1, 2000);
    TEST_ASSERT_EQUAL_INT(0, on_edges);
}

static void test_failed_on_carries_on(void) {
    init_idle();
    fail_next_on = 1;                  // the first beep's "on" fails
    buzzer_start(1000, BUZZER_BEEPS);
    run(1000, 3000);
    TEST_ASSERT_EQUAL_INT(BUZZER_BEEPS - 1, on_edges);
    TEST_ASSERT_FALSE(level);
    TEST_ASSERT_FALSE(buzzer_busy());
}

static void test_millis_wrap(void) {
    // millis() wraps after the first beep: the gaps and beeps must keep their
    // length across it (a plain `now < next` compare would fire the next edge
    // at once).
    const uint32_t start = 0xFFFFFFFFu - 100;
    buzzer_init(fake_set, start - 1000);
    run(start - 1000, start);
    on_edges = 0;
    buzzer_start(start, BUZZER_BEEPS);
    uint32_t t = start;
    uint32_t elapsed = 0, last_rise = 0, last_fall = 0;
    int rises = 0, on_ms = 0;
    bool was = false;
    for (; elapsed < 2000; elapsed++, t++) {
        buzzer_tick(t);
        if (level) on_ms++;
        if (level && !was) {
            if (rises > 0) TEST_ASSERT_EQUAL_UINT32(BUZZER_ON_MS + BUZZER_GAP_MS, elapsed - last_rise);
            else TEST_ASSERT_EQUAL_UINT32(0, elapsed);
            last_rise = elapsed;
            rises++;
        }
        if (!level && was) last_fall = elapsed;
        was = level;
    }
    TEST_ASSERT_EQUAL_INT(BUZZER_BEEPS, rises);
    TEST_ASSERT_EQUAL_INT(BUZZER_BEEPS * BUZZER_ON_MS, on_ms);
    TEST_ASSERT_EQUAL_UINT32(PATTERN_MS, last_fall);
    TEST_ASSERT_FALSE(level);
}

static void test_zero_beeps_is_a_no_op(void) {
    init_idle();
    buzzer_start(1000, 0);
    TEST_ASSERT_FALSE(buzzer_busy());
    run(1000, 2000);
    TEST_ASSERT_EQUAL_INT(0, writes);
}

static void test_no_set_function_is_a_no_op(void) {
    buzzer_init(nullptr, 0);
    buzzer_start(0, BUZZER_BEEPS);
    buzzer_start(0, 0);
    buzzer_tick(0);
    TEST_ASSERT_FALSE(buzzer_busy());
    TEST_ASSERT_EQUAL_INT(0, writes);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_init_switches_off_then_beeps_once);
    RUN_TEST(test_three_beeps_with_their_timing);
    RUN_TEST(test_second_chime_while_playing_is_ignored);
    RUN_TEST(test_failed_off_stops_the_pattern_and_retries);
    RUN_TEST(test_chime_waits_for_a_pending_off);
    RUN_TEST(test_failed_on_carries_on);
    RUN_TEST(test_millis_wrap);
    RUN_TEST(test_zero_beeps_is_a_no_op);
    RUN_TEST(test_no_set_function_is_a_no_op);
    return UNITY_END();
}
