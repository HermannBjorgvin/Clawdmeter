// pair_hold: the touch-hold re-pair gesture on boards without a PWR button.
//   pio test -d firmware -e native -f test_pair_hold
#include <unity.h>
#include "pair_hold.h"

static PairHold h;

void setUp(void) { pair_hold_clear(&h); }
void tearDown(void) {}

// Hold from t0 to t1 in 100 ms steps; count each event.
static int fires, hints, cancels;
static void hold(bool enabled, uint32_t t0, uint32_t t1) {
    for (uint32_t t = t0; t <= t1; t += 100) {
        switch (pair_hold_step(&h, enabled, true, t)) {
        case PAIR_HOLD_FIRE: fires++; break;
        case PAIR_HOLD_HOLDING: hints++; break;
        case PAIR_HOLD_CANCEL: cancels++; break;
        default: break;
        }
    }
}
static PairHoldEvent lift(uint32_t t) { return pair_hold_step(&h, true, false, t); }

static void reset_counts(void) { fires = hints = cancels = 0; }

static void test_five_second_hold_fires_once(void) {
    reset_counts();
    hold(true, 0, 4900);
    TEST_ASSERT_EQUAL(0, fires);
    TEST_ASSERT_EQUAL(1, hints);
    hold(true, 5000, 9000);   // keep holding past the threshold
    TEST_ASSERT_EQUAL(1, fires);
    TEST_ASSERT_EQUAL(PAIR_HOLD_NONE, lift(9100));
    TEST_ASSERT_EQUAL(0, cancels);
}

static void test_a_tap_does_nothing(void) {
    reset_counts();
    hold(true, 0, 200);
    TEST_ASSERT_EQUAL(PAIR_HOLD_NONE, lift(300));
    TEST_ASSERT_EQUAL(0, fires + hints + cancels);
}

static void test_lifting_early_cancels_after_the_hint(void) {
    reset_counts();
    hold(true, 0, 3000);
    TEST_ASSERT_EQUAL(1, hints);
    TEST_ASSERT_EQUAL(PAIR_HOLD_CANCEL, lift(3100));
    // The next hold starts over.
    reset_counts();
    hold(true, 4000, 8900);
    TEST_ASSERT_EQUAL(0, fires);
    hold(true, 9000, 9000);
    TEST_ASSERT_EQUAL(1, fires);
}

static void test_never_counts_while_disabled(void) {
    reset_counts();
    hold(false, 0, 10000);
    TEST_ASSERT_EQUAL(0, fires + hints);
}

static void test_a_press_that_began_disabled_never_counts(void) {
    reset_counts();
    hold(false, 0, 500);          // finger lands while connected
    hold(true, 600, 10000);       // link drops, hint appears, finger still down
    TEST_ASSERT_EQUAL(0, fires + hints);
    lift(10100);
    hold(true, 11000, 16000);     // a fresh press counts
    TEST_ASSERT_EQUAL(1, fires);
}

static void test_hint_disappearing_midway_cancels(void) {
    reset_counts();
    hold(true, 0, 2000);
    hold(false, 2100, 2100);      // connected: hint hidden
    TEST_ASSERT_EQUAL(1, cancels);
    hold(true, 2200, 9000);       // same finger, hint back: still spent
    TEST_ASSERT_EQUAL(0, fires);
}

static void test_survives_millis_wrap(void) {
    reset_counts();
    const uint32_t t0 = 0xFFFFF000UL;
    for (uint32_t i = 0; i <= 52; i++) {
        if (pair_hold_step(&h, true, true, t0 + i * 100) == PAIR_HOLD_FIRE) fires++;
    }
    TEST_ASSERT_EQUAL(1, fires);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_five_second_hold_fires_once);
    RUN_TEST(test_a_tap_does_nothing);
    RUN_TEST(test_lifting_early_cancels_after_the_hint);
    RUN_TEST(test_never_counts_while_disabled);
    RUN_TEST(test_a_press_that_began_disabled_never_counts);
    RUN_TEST(test_hint_disappearing_midway_cancels);
    RUN_TEST(test_survives_millis_wrap);
    return UNITY_END();
}
