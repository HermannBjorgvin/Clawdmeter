// signals: payload → events / activity / sessions, and the old-daemon
// fallback (no fast-clock fields → nothing fires, hooks_seen stays false).
//   pio test -d firmware -e native -f test_signals
#include <unity.h>
#include <stdint.h>
#include "payload.h"
#include "signals.h"
#include "usage_rate.h"

static const uint32_t MIN = 60UL * 1000UL;

void setUp(void) { signals_clear(); }
void tearDown(void) {}

static UsageData parse(const char* json) {
    UsageData d;
    TEST_ASSERT_TRUE(payload_parse(json, &d, nullptr));
    return d;
}

static void test_old_daemon_payload_fires_nothing(void) {
    UsageData d = parse("{\"s\":12,\"sr\":200,\"w\":30,\"wr\":5000,\"st\":\"allowed\",\"ok\":true}");
    SignalsUpdate u = signals_ingest_at(&d, 1000);
    TEST_ASSERT_FALSE(u.event_fired);
    TEST_ASSERT_FALSE(u.session_reset);
    TEST_ASSERT_FALSE(u.sessions_changed);
    TEST_ASSERT_FALSE(u.replay);
    TEST_ASSERT_FALSE(signals_hooks_seen());
    TEST_ASSERT_EQUAL_INT(-1, signals_activity_at(1000));
    TEST_ASSERT_EQUAL_INT(-1, signals_sessions());
}

static void test_first_seq_is_baseline_then_fires_once(void) {
    UsageData d = parse("{\"s\":1,\"e\":\"p\",\"es\":7}");
    TEST_ASSERT_FALSE(signals_ingest_at(&d, 1000).event_fired);   // stale on arrival
    TEST_ASSERT_TRUE(signals_hooks_seen());

    d = parse("{\"s\":1,\"e\":\"s\",\"es\":8}");
    SignalsUpdate u = signals_ingest_at(&d, 2000);
    TEST_ASSERT_TRUE(u.event_fired);
    TEST_ASSERT_EQUAL_CHAR('s', u.event);
    TEST_ASSERT_EQUAL_UINT32(8, u.event_seq);
    TEST_ASSERT_EQUAL_CHAR('s', signals_last_event());

    // Same es again (poll payload carrying the same event): no re-fire.
    TEST_ASSERT_FALSE(signals_ingest_at(&d, 3000).event_fired);
}

// The app sends a new hook event in an `rp:1` write (the usage numbers are
// the last poll's) before the poll lands. That write must fire, and the poll
// carrying the same `es` must not fire it again.
static void test_event_in_replay_fires_once(void) {
    UsageData d = parse("{\"s\":1,\"e\":\"s\",\"es\":1,\"ok\":true}");
    signals_ingest_at(&d, 1000);
    d = parse("{\"s\":1,\"e\":\"p\",\"es\":2,\"rp\":1,\"ok\":true}");
    SignalsUpdate u = signals_ingest_at(&d, 2000);
    TEST_ASSERT_TRUE(u.replay);
    TEST_ASSERT_TRUE(u.event_fired);
    TEST_ASSERT_EQUAL_CHAR('p', u.event);
    TEST_ASSERT_EQUAL_UINT32(2, u.event_seq);
    d = parse("{\"s\":2,\"e\":\"p\",\"es\":2,\"ok\":true}");
    TEST_ASSERT_FALSE(signals_ingest_at(&d, 3000).event_fired);
    // A heartbeat replaying the same event doesn't fire it again either.
    d = parse("{\"s\":2,\"e\":\"p\",\"es\":2,\"rp\":1,\"ok\":true}");
    TEST_ASSERT_FALSE(signals_ingest_at(&d, 4000).event_fired);
}

static void test_seq_restart_still_fires(void) {
    UsageData d = parse("{\"e\":\"p\",\"es\":57}");
    signals_ingest_at(&d, 1000);
    d = parse("{\"e\":\"p\",\"es\":1}");       // daemon restarted its counter
    TEST_ASSERT_TRUE(signals_ingest_at(&d, 2000).event_fired);
}

static void test_activity_lift_in_group(void) {
    UsageData d = parse("{\"s\":10,\"a\":3}");
    SignalsUpdate u = signals_ingest_at(&d, 1000);
    TEST_ASSERT_EQUAL_INT(RATE_IDLE, u.group_before);
    TEST_ASSERT_EQUAL_INT(RATE_HEAVY, u.group_after);
    TEST_ASSERT_EQUAL_INT(3, signals_activity_at(1000));
    // Activity expires if the daemon goes quiet.
    TEST_ASSERT_EQUAL_INT(-1, signals_activity_at(1000 + USAGE_RATE_ACTIVITY_TTL_MS));
}

static void test_replay_skips_rate_but_keeps_activity(void) {
    UsageData d = parse("{\"s\":10,\"a\":1,\"ok\":true}");
    signals_ingest_at(&d, 1000);
    d = parse("{\"s\":40,\"a\":1,\"rp\":1,\"ok\":true}");
    SignalsUpdate u = signals_ingest_at(&d, 1000 + 15 * MIN);
    TEST_ASSERT_EQUAL_INT(RATE_NORMAL, u.group_after);   // lift only; 40% ignored
}

static void test_session_reset_through_signals(void) {
    UsageData d = parse("{\"s\":90,\"ok\":true}");
    signals_ingest_at(&d, 1000);
    d = parse("{\"s\":0,\"rp\":1,\"ok\":true}");
    TEST_ASSERT_FALSE(signals_ingest_at(&d, 2000).session_reset);
    d = parse("{\"s\":0,\"ok\":true}");
    TEST_ASSERT_TRUE(signals_ingest_at(&d, 3000).session_reset);
}

// {"ok":false} "no data" beats (dead token, failed /usage) parse s as 0. They
// must not read as a reset or enter the rate window.
static void test_no_data_beat_is_not_a_sample(void) {
    UsageData d = parse("{\"s\":60,\"ok\":true}");
    signals_ingest_at(&d, 1000);
    d = parse("{\"ok\":false,\"m\":\"Needs you\"}");
    SignalsUpdate u = signals_ingest_at(&d, 2000);
    TEST_ASSERT_FALSE(u.session_reset);
    d = parse("{\"s\":61,\"ok\":true}");
    TEST_ASSERT_FALSE(signals_ingest_at(&d, 3000 + 15 * MIN).session_reset);
    TEST_ASSERT_EQUAL_INT(RATE_IDLE, usage_rate_base_group_at(3000 + 15 * MIN));
}

// The main meter changing what it measures is not a reset: a weekly-only
// account read once as "pro" (0 %, the app couldn't tell after a restart),
// then as "week" again, then an account verified mid-session.
static void test_meter_kind_change_is_not_a_reset(void) {
    UsageData week = parse("{\"s\":0,\"sr\":-1,\"w\":48,\"wr\":7900,\"acct\":\"week\",\"ok\":true}");
    UsageData pro0 = parse("{\"s\":0,\"sr\":-1,\"w\":48,\"wr\":7900,\"acct\":\"pro\",\"ok\":true}");
    UsageData pro70 = parse("{\"s\":70,\"sr\":90,\"w\":48,\"wr\":7900,\"acct\":\"pro\",\"ok\":true}");
    SignalsUpdate u = signals_ingest_at(&week, 1 * MIN);
    TEST_ASSERT_FALSE(u.session_reset);
    TEST_ASSERT_FALSE(u.meter_kind_changed);                                // first sight
    u = signals_ingest_at(&pro0, 20 * MIN);                                 // 48 -> 0
    TEST_ASSERT_FALSE(u.session_reset);
    TEST_ASSERT_TRUE(u.meter_kind_changed);
    TEST_ASSERT_FALSE(signals_ingest_at(&week, 40 * MIN).session_reset);    // back
    TEST_ASSERT_FALSE(signals_ingest_at(&pro70, 60 * MIN).session_reset);
    TEST_ASSERT_FALSE(signals_ingest_at(&week, 80 * MIN).session_reset);    // verified: 70 -> 48
    // A real weekly reset of the same kind still fires.
    UsageData fresh = parse("{\"s\":0,\"sr\":-1,\"w\":0,\"wr\":10080,\"acct\":\"week\",\"ok\":true}");
    u = signals_ingest_at(&fresh, 100 * MIN);
    TEST_ASSERT_TRUE(u.session_reset);
    TEST_ASSERT_FALSE(u.meter_kind_changed);
    // A replay of another kind doesn't count as a sample of that kind.
    UsageData rp = parse("{\"s\":70,\"sr\":90,\"acct\":\"pro\",\"rp\":1,\"ok\":true}");
    u = signals_ingest_at(&rp, 101 * MIN);
    TEST_ASSERT_FALSE(u.session_reset);
    TEST_ASSERT_FALSE(u.meter_kind_changed);
    UsageData later = parse("{\"s\":0,\"sr\":-1,\"w\":1,\"wr\":10000,\"acct\":\"week\",\"ok\":true}");
    u = signals_ingest_at(&later, 120 * MIN);
    TEST_ASSERT_FALSE(u.session_reset);
    TEST_ASSERT_FALSE(u.meter_kind_changed);
}

static void test_sessions_change(void) {
    UsageData d = parse("{\"n\":2}");
    TEST_ASSERT_TRUE(signals_ingest_at(&d, 1000).sessions_changed);
    TEST_ASSERT_FALSE(signals_ingest_at(&d, 2000).sessions_changed);
    TEST_ASSERT_EQUAL_INT(2, signals_sessions());
    d = parse("{\"n\":0}");
    TEST_ASSERT_TRUE(signals_ingest_at(&d, 3000).sessions_changed);
    TEST_ASSERT_EQUAL_INT(0, signals_sessions());
}

// A Codex account's numbers never reach Clawd: not the rate ring, not the
// reset chime, not a rise. Its hook fields (Claude Code's) still fire.
static void test_codex_numbers_never_reach_clawd(void) {
    UsageData claude = parse("{\"s\":60,\"sr\":90,\"w\":30,\"wr\":5000,\"acct\":\"pro\",\"es\":1,\"ok\":true}");
    signals_ingest_at(&claude, 1 * MIN);
    UsageData codex = parse("{\"s\":0,\"sr\":-1,\"w\":2,\"wr\":8000,\"acct\":\"week\",\"pv\":\"codex\","
                            "\"e\":\"p\",\"es\":2,\"ok\":true}");
    SignalsUpdate u = signals_ingest_at(&codex, 2 * MIN);
    TEST_ASSERT_TRUE(u.replay);                 // to Clawd, not a sample
    TEST_ASSERT_FALSE(u.session_reset);         // 60 -> 2 is not a fresh window
    TEST_ASSERT_FALSE(u.meter_kind_changed);
    TEST_ASSERT_TRUE(u.event_fired);            // the prompt still counts
    TEST_ASSERT_EQUAL_CHAR('p', u.event);
    // Back on Claude at the same 60 %: nothing happened in between.
    u = signals_ingest_at(&claude, 3 * MIN);
    TEST_ASSERT_FALSE(u.session_reset);
    TEST_ASSERT_FALSE(u.meter_kind_changed);
    // A real Claude reset still fires.
    UsageData fresh = parse("{\"s\":1,\"sr\":299,\"w\":30,\"wr\":5000,\"acct\":\"pro\",\"ok\":true}");
    TEST_ASSERT_TRUE(signals_ingest_at(&fresh, 4 * MIN).session_reset);
}

// Another account's numbers (`an` changed, a button or the app's choice) are
// a new baseline, not a reset or a rise. A Codex account between two
// readings of the same Claude account changes nothing; an app that sends no
// name never counts as a change.
static void test_account_change_is_not_a_reset(void) {
    UsageData a60 = parse("{\"s\":60,\"sr\":90,\"acct\":\"pro\",\"an\":\"~/.claude\",\"ok\":true}");
    UsageData b8 = parse("{\"s\":8,\"sr\":200,\"acct\":\"pro\",\"an\":\"~/work\",\"ok\":true}");
    SignalsUpdate u = signals_ingest_at(&a60, 1 * MIN);
    TEST_ASSERT_FALSE(u.meter_kind_changed);                 // first sight
    TEST_ASSERT_FALSE(u.account_switched);
    u = signals_ingest_at(&b8, 5 * MIN);                     // 60 -> 8 on another account
    TEST_ASSERT_FALSE(u.session_reset);
    TEST_ASSERT_TRUE(u.meter_kind_changed);
    TEST_ASSERT_TRUE(u.account_switched);
    u = signals_ingest_at(&a60, 6 * MIN);                    // and back: 8 -> 60 is no rise
    TEST_ASSERT_TRUE(u.meter_kind_changed);
    TEST_ASSERT_EQUAL_INT(RATE_IDLE, usage_rate_base_group_at(6 * MIN));
    // Codex in between: named (switched), but A's history stays.
    UsageData cx = parse("{\"s\":0,\"sr\":-1,\"w\":2,\"wr\":8000,\"acct\":\"week\",\"pv\":\"codex\","
                         "\"an\":\"~/.codex\",\"ok\":true}");
    u = signals_ingest_at(&cx, 7 * MIN);
    TEST_ASSERT_TRUE(u.account_switched);
    TEST_ASSERT_FALSE(u.meter_kind_changed);
    u = signals_ingest_at(&a60, 8 * MIN);
    TEST_ASSERT_TRUE(u.account_switched);
    TEST_ASSERT_FALSE(u.meter_kind_changed);                 // A's last real sample was A's
    // A replay naming another account names it, but isn't a sample.
    UsageData b_rp = parse("{\"s\":8,\"acct\":\"pro\",\"an\":\"~/work\",\"rp\":1,\"ok\":true}");
    u = signals_ingest_at(&b_rp, 9 * MIN);
    TEST_ASSERT_TRUE(u.account_switched);
    TEST_ASSERT_FALSE(u.session_reset);
    TEST_ASSERT_FALSE(u.meter_kind_changed);
    // The same account picked another way (`al`: the latest active one, then
    // chosen) is named again, but its numbers stay comparable.
    UsageData b_auto = parse("{\"s\":9,\"sr\":190,\"acct\":\"pro\",\"an\":\"~/work\",\"al\":1,\"ok\":true}");
    u = signals_ingest_at(&b_auto, 9 * MIN + 30000);
    TEST_ASSERT_TRUE(u.account_switched);
    TEST_ASSERT_TRUE(u.meter_kind_changed);                  // ~/.claude's last real sample before it
    UsageData b_pin = parse("{\"s\":9,\"sr\":190,\"acct\":\"pro\",\"an\":\"~/work\",\"ok\":true}");
    u = signals_ingest_at(&b_pin, 9 * MIN + 40000);
    TEST_ASSERT_TRUE(u.account_switched);
    TEST_ASSERT_FALSE(u.meter_kind_changed);                 // same account's numbers
    u = signals_ingest_at(&b_pin, 9 * MIN + 50000);
    TEST_ASSERT_FALSE(u.account_switched);
    // Down to one account (the app names none): another account's numbers,
    // a new baseline, not named. Its own reset then fires as ever.
    UsageData plain = parse("{\"s\":1,\"sr\":299,\"acct\":\"pro\",\"ok\":true}");
    u = signals_ingest_at(&plain, 10 * MIN);
    TEST_ASSERT_FALSE(u.account_switched);
    TEST_ASSERT_TRUE(u.meter_kind_changed);
    TEST_ASSERT_FALSE(u.session_reset);                      // 9 -> 1, but ~/work's 9
    UsageData plain60 = parse("{\"s\":60,\"sr\":200,\"acct\":\"pro\",\"ok\":true}");
    UsageData plain0 = parse("{\"s\":0,\"sr\":299,\"acct\":\"pro\",\"ok\":true}");
    signals_ingest_at(&plain60, 11 * MIN);
    u = signals_ingest_at(&plain0, 12 * MIN);
    TEST_ASSERT_TRUE(u.session_reset);
    TEST_ASSERT_FALSE(u.meter_kind_changed);
}

// A second account added: the app starts naming accounts, so the first named
// sample is a new baseline even when it's another account's (60 -> 5 is no
// reset). An older app never names one, so its resets fire as ever
// (test_session_reset_through_signals).
static void test_adding_an_account_is_not_a_reset(void) {
    UsageData one = parse("{\"s\":60,\"sr\":90,\"acct\":\"pro\",\"ok\":true}");
    UsageData other = parse("{\"s\":5,\"sr\":290,\"acct\":\"pro\",\"an\":\"~/b\",\"ok\":true}");
    signals_ingest_at(&one, 1 * MIN);
    SignalsUpdate u = signals_ingest_at(&other, 5 * MIN);
    TEST_ASSERT_FALSE(u.session_reset);
    TEST_ASSERT_TRUE(u.meter_kind_changed);
    TEST_ASSERT_FALSE(u.account_switched);                   // the first name is a baseline
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_account_change_is_not_a_reset);
    RUN_TEST(test_adding_an_account_is_not_a_reset);
    RUN_TEST(test_codex_numbers_never_reach_clawd);
    RUN_TEST(test_old_daemon_payload_fires_nothing);
    RUN_TEST(test_first_seq_is_baseline_then_fires_once);
    RUN_TEST(test_event_in_replay_fires_once);
    RUN_TEST(test_seq_restart_still_fires);
    RUN_TEST(test_activity_lift_in_group);
    RUN_TEST(test_replay_skips_rate_but_keeps_activity);
    RUN_TEST(test_session_reset_through_signals);
    RUN_TEST(test_no_data_beat_is_not_a_sample);
    RUN_TEST(test_sessions_change);
    RUN_TEST(test_meter_kind_change_is_not_a_reset);
    return UNITY_END();
}
