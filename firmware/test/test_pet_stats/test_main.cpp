// pet stats: what feeds the ASCII pet (level from usage rises, mood from how
// alerts are answered, energy from naps).
//   pio test -d firmware -e native -f test_pet_stats
#include <unity.h>
#include "pet/pet_stats_core.h"

void setUp(void) {}
void tearDown(void) {}

static PetStats fresh(void) {
    PetStats s;
    pet_stats_clear(&s);
    return s;
}

static void test_first_reading_is_a_baseline(void) {
    PetStats s = fresh();
    TEST_ASSERT_FALSE(pet_stats_usage(&s, 42.0f, true, false));
    TEST_ASSERT_EQUAL_UINT32(0, s.points_x100);
}

static void test_rises_are_eaten(void) {
    PetStats s = fresh();
    pet_stats_usage(&s, 10.0f, true, false);
    pet_stats_usage(&s, 12.5f, true, false);
    pet_stats_usage(&s, 12.5f, true, false);     // no change
    pet_stats_usage(&s, 60.0f, true, false);
    TEST_ASSERT_EQUAL_UINT32(5000, s.points_x100);   // 50 points
    TEST_ASSERT_EQUAL(0, pet_stats_level(&s));
    TEST_ASSERT_EQUAL(1, pet_stats_fed(&s));         // one pip per 50 points
}

static void test_replays_resets_and_kind_changes_feed_nothing(void) {
    PetStats s = fresh();
    pet_stats_usage(&s, 10.0f, true, false);
    pet_stats_usage(&s, 90.0f, false, false);    // a replay (or a Codex account)
    TEST_ASSERT_EQUAL_UINT32(0, s.points_x100);
    pet_stats_usage(&s, 30.0f, true, false);     // 10 -> 30: the replay moved no baseline
    TEST_ASSERT_EQUAL_UINT32(2000, s.points_x100);
    pet_stats_usage(&s, 0.0f, true, false);      // the window reset
    TEST_ASSERT_EQUAL_UINT32(2000, s.points_x100);
    pet_stats_usage(&s, 5.0f, true, false);      // 0 -> 5 after the reset
    TEST_ASSERT_EQUAL_UINT32(2500, s.points_x100);
    pet_stats_usage(&s, 70.0f, true, true);      // now the weekly meter: a new baseline
    TEST_ASSERT_EQUAL_UINT32(2500, s.points_x100);
    pet_stats_usage(&s, 71.0f, true, false);
    TEST_ASSERT_EQUAL_UINT32(2600, s.points_x100);
}

static void test_a_weekly_meter_weighs_five_times(void) {
    PetStats s = fresh();
    pet_stats_usage(&s, 0.0f, true, false, PET_WEEK_WEIGHT);
    pet_stats_usage(&s, 10.0f, true, false, PET_WEEK_WEIGHT);
    TEST_ASSERT_EQUAL(1, pet_stats_fed(&s));                 // a pip per 10 % of the week
    TEST_ASSERT_FALSE(pet_stats_usage(&s, 99.0f, true, false, PET_WEEK_WEIGHT));
    TEST_ASSERT_TRUE(pet_stats_usage(&s, 100.0f, true, false, PET_WEEK_WEIGHT));
    TEST_ASSERT_EQUAL(1, pet_stats_level(&s));               // a level per full week
}

static void test_level_up_is_reported_once(void) {
    PetStats s = fresh();
    s.points_x100 = (PET_POINTS_PER_LEVEL - 1) * 100u;
    pet_stats_usage(&s, 0.0f, true, false);
    TEST_ASSERT_FALSE(pet_stats_usage(&s, 0.5f, true, false));
    TEST_ASSERT_TRUE(pet_stats_usage(&s, 2.0f, true, false));    // crosses the level
    TEST_ASSERT_FALSE(pet_stats_usage(&s, 3.0f, true, false));
    TEST_ASSERT_EQUAL(1, pet_stats_level(&s));
    TEST_ASSERT_EQUAL(0, pet_stats_fed(&s));
}

static void test_fed_stays_under_its_pips(void) {
    PetStats s = fresh();
    s.points_x100 = PET_POINTS_PER_LEVEL * 100u - 1;
    TEST_ASSERT_EQUAL(PET_FED_PIPS - 1, pet_stats_fed(&s));
}

static void test_median_velocity(void) {
    PetStats s = fresh();
    TEST_ASSERT_EQUAL(0, pet_stats_median_velocity(&s));
    pet_stats_answered(&s, 40);
    pet_stats_answered(&s, 5);
    pet_stats_answered(&s, 300);
    TEST_ASSERT_EQUAL(40, pet_stats_median_velocity(&s));
    for (int i = 0; i < 8; i++) pet_stats_answered(&s, 10);   // the ring holds the last 8
    TEST_ASSERT_EQUAL(10, pet_stats_median_velocity(&s));
    TEST_ASSERT_EQUAL(11, s.answered);
}

static void test_mood_follows_answer_speed(void) {
    PetStats s = fresh();
    TEST_ASSERT_EQUAL(2, pet_stats_mood(&s));                 // no data: neutral
    pet_stats_answered(&s, 8);
    TEST_ASSERT_EQUAL(4, pet_stats_mood(&s));
    s = fresh(); pet_stats_answered(&s, 20);
    TEST_ASSERT_EQUAL(3, pet_stats_mood(&s));
    s = fresh(); pet_stats_answered(&s, 45);
    TEST_ASSERT_EQUAL(2, pet_stats_mood(&s));
    s = fresh(); pet_stats_answered(&s, 100);
    TEST_ASSERT_EQUAL(1, pet_stats_mood(&s));
    s = fresh(); pet_stats_answered(&s, 119);
    TEST_ASSERT_EQUAL(1, pet_stats_mood(&s));
}

static void test_ignored_alerts_lower_the_mood(void) {
    PetStats s = fresh();
    for (int i = 0; i < 4; i++) pet_stats_answered(&s, 5);    // tier 4
    pet_stats_ignored(&s);
    TEST_ASSERT_EQUAL(4, pet_stats_mood(&s));                 // 1 of 5: under a third
    pet_stats_ignored(&s);
    pet_stats_ignored(&s);
    TEST_ASSERT_EQUAL(3, pet_stats_mood(&s));                 // 3 ignored vs 4 answered: over a third
    pet_stats_ignored(&s);
    pet_stats_ignored(&s);
    TEST_ASSERT_EQUAL(2, pet_stats_mood(&s));                 // more ignored than answered
    s = fresh();
    for (int i = 0; i < 5; i++) pet_stats_ignored(&s);
    TEST_ASSERT_EQUAL(0, pet_stats_mood(&s));                 // never below 0
}

static void test_energy(void) {
    const uint32_t H = 3600000u;
    PetEnergy e;
    pet_energy_boot(&e, 1000);
    TEST_ASSERT_EQUAL(3, pet_energy_tier(&e, 1000));
    TEST_ASSERT_EQUAL(3, pet_energy_tier(&e, 1000 + 2 * H - 1));
    TEST_ASSERT_EQUAL(2, pet_energy_tier(&e, 1000 + 2 * H));
    TEST_ASSERT_EQUAL(0, pet_energy_tier(&e, 1000 + 6 * H));
    TEST_ASSERT_EQUAL(0, pet_energy_tier(&e, 1000 + 40 * H));  // never below 0
    pet_energy_refill(&e, 1000 + 40 * H);
    TEST_ASSERT_EQUAL(PET_ENERGY_MAX, pet_energy_tier(&e, 1000 + 40 * H));
    TEST_ASSERT_EQUAL(PET_ENERGY_MAX - 1, pet_energy_tier(&e, 1000 + 42 * H));
}

static void test_energy_across_millis_wrap(void) {
    PetEnergy e;
    pet_energy_refill(&e, 0xFFFFF000u);
    TEST_ASSERT_EQUAL(PET_ENERGY_MAX, pet_energy_tier(&e, 0x00000100u));   // 4 s later, past the wrap
}

static void test_nap_adds_up(void) {
    PetStats s = fresh();
    pet_stats_nap(&s, 300);
    pet_stats_nap(&s, 45);
    TEST_ASSERT_EQUAL_UINT32(345, s.nap_seconds);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_first_reading_is_a_baseline);
    RUN_TEST(test_rises_are_eaten);
    RUN_TEST(test_replays_resets_and_kind_changes_feed_nothing);
    RUN_TEST(test_a_weekly_meter_weighs_five_times);
    RUN_TEST(test_level_up_is_reported_once);
    RUN_TEST(test_fed_stays_under_its_pips);
    RUN_TEST(test_median_velocity);
    RUN_TEST(test_mood_follows_answer_speed);
    RUN_TEST(test_ignored_alerts_lower_the_mood);
    RUN_TEST(test_energy);
    RUN_TEST(test_energy_across_millis_wrap);
    RUN_TEST(test_nap_adds_up);
    return UNITY_END();
}
