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

static void test_replay_never_fires(void) {
    UsageData d = parse("{\"s\":1,\"e\":\"p\",\"es\":1}");
    signals_ingest_at(&d, 1000);
    d = parse("{\"s\":1,\"e\":\"b\",\"es\":2,\"rp\":1}");
    SignalsUpdate u = signals_ingest_at(&d, 2000);
    TEST_ASSERT_TRUE(u.replay);
    TEST_ASSERT_FALSE(u.event_fired);
    // ...and the replayed seq is consumed, so the live copy doesn't fire late.
    d = parse("{\"s\":1,\"e\":\"b\",\"es\":2}");
    TEST_ASSERT_FALSE(signals_ingest_at(&d, 3000).event_fired);
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

static void test_sessions_change(void) {
    UsageData d = parse("{\"n\":2}");
    TEST_ASSERT_TRUE(signals_ingest_at(&d, 1000).sessions_changed);
    TEST_ASSERT_FALSE(signals_ingest_at(&d, 2000).sessions_changed);
    TEST_ASSERT_EQUAL_INT(2, signals_sessions());
    d = parse("{\"n\":0}");
    TEST_ASSERT_TRUE(signals_ingest_at(&d, 3000).sessions_changed);
    TEST_ASSERT_EQUAL_INT(0, signals_sessions());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_old_daemon_payload_fires_nothing);
    RUN_TEST(test_first_seq_is_baseline_then_fires_once);
    RUN_TEST(test_replay_never_fires);
    RUN_TEST(test_seq_restart_still_fires);
    RUN_TEST(test_activity_lift_in_group);
    RUN_TEST(test_replay_skips_rate_but_keeps_activity);
    RUN_TEST(test_session_reset_through_signals);
    RUN_TEST(test_no_data_beat_is_not_a_sample);
    RUN_TEST(test_sessions_change);
    return UNITY_END();
}
