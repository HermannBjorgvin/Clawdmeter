// ding: the synthesized chime (pitch, attack, strike, ring, click, level).
//   pio test -d firmware -e native -f test_ding
// The golden frames are shared with the desktop's chime.rs tests: the two
// must make the same sound.
#include <unity.h>
#include <math.h>
#include "ding.h"

static const uint32_t RATE = 44100;

void setUp(void) {}
void tearDown(void) {}

static float peak(uint32_t a, uint32_t b) {
    float p = 0;
    for (uint32_t n = a; n < b; n++) p = fmaxf(p, fabsf(ding_value(n, RATE)));
    return p;
}

static void test_length(void) {
    TEST_ASSERT_EQUAL_UINT32(73647, ding_frames(RATE));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, ding_value(ding_frames(RATE), RATE));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, ding_value(0, 0));
    // The ring is down to about -65 dB at the cut.
    TEST_ASSERT_TRUE(peak(ding_frames(RATE) - 200, ding_frames(RATE)) < 0.001f);
}

static void test_strike_peaks_just_under_full_scale(void) {
    const float p = peak(0, RATE / 10);
    TEST_ASSERT_TRUE(p > 0.9f && p < 1.0f);   // about -0.4 dBFS
    // The attack is under a millisecond: near full strike by 1.5 ms.
    TEST_ASSERT_TRUE(peak(0, 10) < 0.2f);
    TEST_ASSERT_TRUE(peak(40, 80) > 0.9f);
}

static void test_two_stage_decay(void) {
    // The fast stage: 50 ms in, the level has dropped well below the strike.
    const uint32_t period = (uint32_t)(RATE / 554.5f) + 1;
    TEST_ASSERT_TRUE(peak(RATE / 20, RATE / 20 + period) < 0.6f * peak(40, 80));
    // The slow ring: one 0.24 s time constant later, 1/e of the level.
    const uint32_t t = RATE / 10, tau = (uint32_t)(0.24f * RATE);
    const float ratio = peak(t + tau, t + tau + period) / peak(t, t + period);
    TEST_ASSERT_FLOAT_WITHIN(0.03f, 1.0f / 2.71828f, ratio);
}

static void test_rings_at_c_sharp_5(void) {
    int rising = 0;
    for (uint32_t n = RATE / 50; n < RATE / 50 + RATE / 2; n++)
        if (ding_value(n - 1, RATE) < 0 && ding_value(n, RATE) >= 0) rising++;
    TEST_ASSERT_INT_WITHIN(2, 277, rising);
}

static void test_golden_frames(void) {
    // Strike with the click, then the ring (matches the reference tone).
    const struct { uint32_t n; float v; } golden[] = {
        {20, 0.588196f}, {100, 0.892679f}, {441, -0.209996f},
        {1000, -0.269436f}, {4410, 0.131246f}, {10000, -0.231227f},
    };
    for (unsigned i = 0; i < sizeof golden / sizeof golden[0]; i++)
        TEST_ASSERT_FLOAT_WITHIN(2e-4f, golden[i].v, ding_value(golden[i].n, RATE));
}

static void test_samples_scale_and_clamp(void) {
    const uint32_t n = 100;
    TEST_ASSERT_EQUAL_INT16(0, ding_sample(n, RATE, 0.0f));
    const int16_t full = ding_sample(n, RATE, 1.0f);
    TEST_ASSERT_EQUAL_INT16(full, ding_sample(n, RATE, 2.0f));
    TEST_ASSERT_INT_WITHIN(1, full / 2, ding_sample(n, RATE, 0.5f));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_length);
    RUN_TEST(test_strike_peaks_just_under_full_scale);
    RUN_TEST(test_two_stage_decay);
    RUN_TEST(test_rings_at_c_sharp_5);
    RUN_TEST(test_golden_frames);
    RUN_TEST(test_samples_scale_and_clamp);
    return UNITY_END();
}
