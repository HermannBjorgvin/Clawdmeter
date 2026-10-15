// The director against timed stand-ins for its actors (fake_actors.cpp):
// whatever the situation does, there is never more than one Clawd on screen
// (corner sprite, full-size sprite, performer, face).
//   pio test -d firmware -e native -f test_director
#include <unity.h>
#include "fake_world.h"
#include "../../src/clawd_director.cpp"

static const uint32_t TICK_MS = 10;

// The first frame that broke the rule, if any.
static bool     broke = false;
static uint32_t broke_at = 0;
static char     broke_what[96];

static void check_one_clawd(void) {
    const OnScreen s = fake_on_screen();
    if (broke || fake_clawds(s) <= 1) return;
    broke = true;
    broke_at = fake_now_ms;
    snprintf(broke_what, sizeof(broke_what), "beat %s:%u corner %d big %d performer %d face %d",
             director_beat_name(), step, s.corner, s.big, s.performer, s.face);
}

// Which beats a run went through, so a fuzz pass can't be vacuous.
static const int BEAT_COUNT = sizeof(BEAT_NAMES) / sizeof(BEAT_NAMES[0]);
static bool seen[BEAT_COUNT];

static void run_for(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += TICK_MS) {
        fake_now_ms += TICK_MS;
        fake_actors_tick();
        director_tick();
        seen[beat] = true;
        check_one_clawd();
    }
}

// Without the full-size sprite nothing performs, so nothing runs home.
static void assert_every_beat_seen(bool has_big) {
    for (int b = 0; b < BEAT_COUNT; b++)
        if (has_big || b != B_RUN_HOME) TEST_ASSERT_TRUE_MESSAGE(seen[b], BEAT_NAMES[b]);
}

static void assert_one_clawd(void) {
    if (!broke) return;
    char msg[160];
    snprintf(msg, sizeof(msg), "two Clawds at %lu ms: %s", (unsigned long)broke_at, broke_what);
    TEST_FAIL_MESSAGE(msg);
}

static UsageData live_payload(void) {
    UsageData d;
    memset(&d, 0, sizeof(d));
    d.valid = true;
    d.ok = true;
    d.session_pct = 10;
    d.session_reset_mins = 120;
    d.activity = -1;
    d.sessions = -1;
    return d;
}

static void send(const UsageData& d, const SignalsUpdate& sig) { director_on_payload(&d, &sig); }

static SignalsUpdate no_signal(void) {
    SignalsUpdate s;
    memset(&s, 0, sizeof(s));
    return s;
}

static void fresh_director(void);
static int fuzz_attn_s = CLAWD_ATTN_ANIM_DEFAULT_S;

void setUp(void) {
    broke = false;
    world = { true, 0, false, true };
}
void tearDown(void) {}

// The screensaver rests, then dips into a calm scene. The face must be off
// screen before the performer appears, and he must be gone before it rises.
static void test_screensaver_calm_dip_keeps_one_clawd(void) {
    fresh_director();
    run_for(15 * 1000 + 2000);                    // quiet: the screensaver starts
    TEST_ASSERT_EQUAL_STRING("screensaver", director_beat_name());
    run_for(60 * 1000);                           // rest, dip, rest, dip, ...
    assert_one_clawd();
}

// Seeded fuzz: activity, prompts, stops, usage moving, fresh windows,
// alerts and clears, taps, buttons, the link dropping, and the QA beats, at
// random moments over a long stretch.
static uint32_t fz = 0xC1A3D;
static uint32_t fz_next(void) { fz ^= fz << 13; fz ^= fz >> 17; fz ^= fz << 5; return fz; }
static uint32_t fz_range(uint32_t lo, uint32_t hi) { return lo + fz_next() % (hi - lo + 1); }

// director_init() is for boot; between runs, also drop what a run left
// pending (this file includes the director, so its statics are in reach).
static void fresh_director(void) {
    attention_on = false;
    resume_screensaver = false;
    react_anim = NULL;
    joy_pending = false;
    ClawdTuning fast;
    clawd_tuning_fast(&fast);
    clawd_tuning_set_base(&fast);
    clawd_set_liveliness(CLAWD_LIVELINESS_DEFAULT);
    clawd_set_attn_anim(fuzz_attn_s);
    director_init();
    // Hooks installed: one activity reading is enough for signals to know.
    signals_clear();
    UsageData d = live_payload();
    d.activity = 0;
    signals_ingest_at(&d, fake_now_ms);
}

static int fuzz_level = CLAWD_LIVELINESS_DEFAULT;

static void fuzz(uint32_t seed, uint32_t total_ms, bool has_big) {
    fz = seed;
    world.has_big = has_big;
    fresh_director();
    director_set_liveliness(fuzz_level);
    float pct = 10;
    uint32_t es = 0;
    for (uint32_t t = 0; t < total_ms && !broke;) {
        const uint32_t gap = fz_range(50, 6000);
        run_for(gap);
        t += gap;
        UsageData d = live_payload();
        SignalsUpdate sig = no_signal();
        switch (fz_next() % 16) {
        case 0: case 1:  world.activity = (int)fz_range(0, 3); break;
        case 2:          world.activity = 3; break;
        case 3: case 4:  sig.event_fired = true; sig.event = "psb"[fz_next() % 3]; sig.event_seq = ++es; break;
        case 5:          pct += 1; d.session_pct = pct; break;
        case 6:          sig.session_reset = true; pct = 0; d.session_pct = pct; break;
        case 7:          director_alert("needs you"); continue;
        case 8:          director_alert_clear(); continue;
        case 9: case 10: director_tap((int)fz_range(0, 479)); continue;
        case 11:         (void)director_button(true); continue;
        case 12:         world.meters_live = fz_next() % 4 != 0; continue;
        case 13: {
            static const char* const BEATS[] = { "show", "screensaver", "lurk", "peek", "joy", "home" };
            director_debug(BEATS[fz_next() % 6]);
            continue;
        }
        case 14:         director_set_liveliness((int)fz_range(1, 5)); continue;
        default:         break;                   // a quiet stretch
        }
        d.session_pct = pct;
        send(d, sig);
    }
    assert_one_clawd();
    // Left alone afterwards, nothing hangs: whatever was running finishes
    // and he ends up home or in the screensaver.
    director_alert_clear();
    world.meters_live = true;
    world.activity = 0;
    run_for(3 * 60 * 1000);
    assert_one_clawd();
    const char* b = director_beat_name();
    char msg[64];
    snprintf(msg, sizeof(msg), "seed %lu stuck in %s", (unsigned long)seed, b);
    TEST_ASSERT_TRUE_MESSAGE(strcmp(b, "home") == 0 || strcmp(b, "screensaver") == 0, msg);
}

static void test_fuzz_one_clawd_with_full_size_sprite(void) {
    memset(seen, 0, sizeof(seen));
    for (uint32_t seed = 1; seed <= 40 && !broke; seed++) fuzz(seed * 2654435761u, 30 * 60 * 1000, true);
    assert_every_beat_seen(true);
}

static void test_fuzz_one_clawd_without_full_size_sprite(void) {
    memset(seen, 0, sizeof(seen));
    for (uint32_t seed = 1; seed <= 20 && !broke; seed++) fuzz(seed * 40503u, 30 * 60 * 1000, false);
    assert_every_beat_seen(false);
}

// No-nonsense: everything that isn't a tap or an alert leaves him sitting
// still in his seat: busy stretches, prompts, stops, usage moving, a fresh
// window, a long quiet.
static void test_no_nonsense_sits_still_through_everything_but_taps(void) {
    fresh_director();
    director_set_liveliness(1);
    float pct = 10;
    uint32_t es = 0;
    world.activity = 3;
    bool moved = false;
    for (int i = 0; i < 400 && !moved; i++) {
        UsageData d = live_payload();
        SignalsUpdate sig = no_signal();
        if (i % 3 == 0) { sig.event_fired = true; sig.event = "psb"[(i / 3) % 3]; sig.event_seq = ++es; }
        if (i % 7 == 0) pct += 1;
        if (i == 200) { sig.session_reset = true; pct = 0; }
        d.session_pct = pct;
        send(d, sig);
        for (int t = 0; t < 500; t += TICK_MS) {    // 200 s in all, past every fast timer
            run_for(TICK_MS);
            if (!fake_corner_seated_still() || strcmp(director_beat_name(), "home") != 0) moved = true;
        }
    }
    TEST_ASSERT_FALSE_MESSAGE(moved, "he left his seat or a beat started");
    director_tap(300);                               // a tap still gets the peek
    run_for(3000);
    TEST_ASSERT_EQUAL_STRING("peek", director_beat_name());
    director_set_liveliness(CLAWD_LIVELINESS_DEFAULT);
}

// Turning the slider down mid-show ends the show gracefully.
static void test_no_nonsense_ends_a_running_show(void) {
    fresh_director();
    TEST_ASSERT_TRUE(director_debug("show"));
    run_for(6000);
    director_set_liveliness(1);
    run_for(30 * 1000);
    assert_one_clawd();
    TEST_ASSERT_EQUAL_STRING("home", director_beat_name());
    TEST_ASSERT_TRUE(fake_corner_seated_still());
    director_set_liveliness(CLAWD_LIVELINESS_DEFAULT);
}

static void test_fuzz_one_clawd_at_every_liveliness(void) {
    for (fuzz_level = 1; fuzz_level <= 5 && !broke; fuzz_level++)
        for (uint32_t seed = 1; seed <= 6 && !broke; seed++) fuzz(seed * 97u + fuzz_level, 20 * 60 * 1000, true);
    fuzz_level = CLAWD_LIVELINESS_DEFAULT;
    assert_one_clawd();
}

// Attention animation Off: the alert is up (message, dismissing tap, the
// app hears about it) but he never points or waves; he sits in his seat,
// including when the alert cut a show short and he had to come back.
static void attention_off(bool has_big) {
    world.has_big = has_big;
    fuzz_attn_s = 0;
    fresh_director();
    world.activity = 3;
    TEST_ASSERT_TRUE(director_debug("show"));
    run_for(has_big ? 9000 : 1500);              // mid-performance (or mid corner act)
    director_alert("needs you");
    bool animated = false;
    for (int t = 0; t < 15000; t += TICK_MS) {   // under the sim's 20 s alert
        run_for(TICK_MS);
        if (fake_attention_animating()) animated = true;
    }
    TEST_ASSERT_FALSE_MESSAGE(animated, "he pointed or waved with the animation off");
    TEST_ASSERT_EQUAL_STRING("attention", director_beat_name());
    TEST_ASSERT_TRUE(attention_on);
    TEST_ASSERT_TRUE(fake_corner_seated_still());
    director_tap(200);                           // the alert itself still works
    TEST_ASSERT_TRUE(director_take_dismissed());
    run_for(5000);
    TEST_ASSERT_EQUAL_STRING("home", director_beat_name());
    assert_one_clawd();
    fuzz_attn_s = CLAWD_ATTN_ANIM_DEFAULT_S;
}
// Without a touchscreen BOOT is the dismiss gesture: it ends an alert (and
// tells the app) instead of doing its normal job; with nothing to dismiss, or
// with a touchscreen, the press is left to its normal role.
static void test_boot_dismisses_alert_without_touch(void) {
    world.has_big = true;
    world.has_touch = false;
    fresh_director();
    TEST_ASSERT_FALSE_MESSAGE(director_button(true), "nothing to dismiss");
    director_alert("needs you");
    run_for(1000);
    TEST_ASSERT_TRUE(attention_on);
    TEST_ASSERT_FALSE_MESSAGE(director_button(false), "a secondary button is not the dismiss gesture");
    TEST_ASSERT_TRUE(attention_on);
    TEST_ASSERT_TRUE_MESSAGE(director_button(true), "BOOT dismisses the alert");
    TEST_ASSERT_FALSE(attention_on);
    TEST_ASSERT_TRUE(director_take_dismissed());
    run_for(5000);
    TEST_ASSERT_EQUAL_STRING("home", director_beat_name());
    TEST_ASSERT_FALSE_MESSAGE(director_button(true), "back to push to talk");
    world.has_touch = true;
}

static void test_boot_leaves_alert_alone_with_touch(void) {
    world.has_big = true;
    world.has_touch = true;
    fresh_director();
    director_alert("needs you");
    run_for(1000);
    TEST_ASSERT_FALSE(director_button(true));
    TEST_ASSERT_TRUE(attention_on);
}

static void test_attention_animation_off_with_full_size_sprite(void) { attention_off(true); }
static void test_attention_animation_off_without_full_size_sprite(void) { attention_off(false); }

// A short attention animation (10 s: 1.7 s of the sim's 20 s alert): he
// points and waves, then goes back to his seat while the alert stays up
// until its own timeout.
static void attention_short(bool has_big) {
    world.has_big = has_big;
    fuzz_attn_s = 10;
    fresh_director();
    director_alert("needs you");
    run_for(1000);
    TEST_ASSERT_TRUE(fake_attention_animating());
    run_for(6000);                               // his part is over, and he's home
    TEST_ASSERT_FALSE(fake_attention_animating());
    TEST_ASSERT_TRUE(fake_corner_seated_still());
    TEST_ASSERT_TRUE(attention_on);
    TEST_ASSERT_EQUAL_STRING("attention", director_beat_name());
    run_for(8000);                               // 15 s in: still sitting, alert still up
    TEST_ASSERT_TRUE(fake_corner_seated_still());
    TEST_ASSERT_TRUE(attention_on);
    director_alert("needs you again");           // a new alert animates again
    run_for(1000);
    TEST_ASSERT_TRUE(fake_attention_animating());
    run_for(25 * 1000);                          // the alert times out (nobody came: the
    TEST_ASSERT_FALSE(attention_on);             // quiet time may have opened the screensaver)
    TEST_ASSERT_TRUE(director_take_dismissed());
    TEST_ASSERT_TRUE(strcmp(director_beat_name(), "attention") != 0);
    assert_one_clawd();
    fuzz_attn_s = CLAWD_ATTN_ANIM_DEFAULT_S;
}
static void test_short_attention_animation_with_full_size_sprite(void) { attention_short(true); }
static void test_short_attention_animation_without_full_size_sprite(void) { attention_short(false); }

static void test_fuzz_one_clawd_at_every_attention_animation(void) {
    static const int SETTINGS[] = { 0, 10, 30 };
    for (int i = 0; i < 3 && !broke; i++) {
        fuzz_attn_s = SETTINGS[i];
        for (uint32_t seed = 1; seed <= 6 && !broke; seed++) {
            fuzz(seed * 7919u + i, 20 * 60 * 1000, true);
            fuzz(seed * 104729u + i, 20 * 60 * 1000, false);
        }
    }
    fuzz_attn_s = CLAWD_ATTN_ANIM_DEFAULT_S;
    assert_one_clawd();
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_screensaver_calm_dip_keeps_one_clawd);
    RUN_TEST(test_fuzz_one_clawd_with_full_size_sprite);
    RUN_TEST(test_fuzz_one_clawd_without_full_size_sprite);
    RUN_TEST(test_no_nonsense_sits_still_through_everything_but_taps);
    RUN_TEST(test_no_nonsense_ends_a_running_show);
    RUN_TEST(test_fuzz_one_clawd_at_every_liveliness);
    RUN_TEST(test_boot_dismisses_alert_without_touch);
    RUN_TEST(test_boot_leaves_alert_alone_with_touch);
    RUN_TEST(test_attention_animation_off_with_full_size_sprite);
    RUN_TEST(test_attention_animation_off_without_full_size_sprite);
    RUN_TEST(test_short_attention_animation_with_full_size_sprite);
    RUN_TEST(test_short_attention_animation_without_full_size_sprite);
    RUN_TEST(test_fuzz_one_clawd_at_every_attention_animation);
    return UNITY_END();
}
