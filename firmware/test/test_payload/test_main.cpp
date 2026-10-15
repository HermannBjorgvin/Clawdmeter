// payload_parse: long-standing fields keep their defaults, fast-clock fields
// are optional and clamped.
//   pio test -d firmware -e native -f test_payload
#include <unity.h>
#include <string.h>
#include "payload.h"

void setUp(void) {}
void tearDown(void) {}

static void test_rejects_malformed_json(void) {
    UsageData d = {};
    d.session_pct = 42;
    const char* err = nullptr;
    TEST_ASSERT_FALSE(payload_parse("{\"s\":", &d, &err));
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_EQUAL_FLOAT(42, d.session_pct);   // untouched on failure
}

static void test_empty_object_gives_legacy_defaults(void) {
    UsageData d;
    TEST_ASSERT_TRUE(payload_parse("{}", &d, nullptr));
    TEST_ASSERT_EQUAL_FLOAT(0, d.session_pct);
    TEST_ASSERT_EQUAL_INT(-1, d.session_reset_mins);
    TEST_ASSERT_EQUAL_INT(-1, d.weekly_reset_mins);
    TEST_ASSERT_EQUAL_STRING("unknown", d.status);
    TEST_ASSERT_FALSE(d.chime);
    TEST_ASSERT_FALSE(d.beep);
    TEST_ASSERT_EQUAL_STRING("", d.alert_msg);
    TEST_ASSERT_FALSE(d.enterprise);
    TEST_ASSERT_EQUAL_INT(30, d.period_days);
    TEST_ASSERT_EQUAL_INT(24, d.clock_fmt);
    TEST_ASSERT_FALSE(d.ok);
    TEST_ASSERT_TRUE(d.valid);
    // fast clock absent
    TEST_ASSERT_EQUAL_INT(-1, d.activity);
    TEST_ASSERT_EQUAL_CHAR('\0', d.event);
    TEST_ASSERT_FALSE(d.has_event_seq);
    TEST_ASSERT_EQUAL_INT(-1, d.sessions);
    TEST_ASSERT_FALSE(d.replay);
}

static void test_legacy_fields(void) {
    UsageData d;
    TEST_ASSERT_TRUE(payload_parse(
        "{\"s\":12.5,\"sr\":240,\"w\":38,\"wr\":1500,\"st\":\"allowed\",\"c\":1,\"b\":1,"
        "\"m\":\"Claude needs your permission\",\"x\":1,\"acct\":\"ent\",\"tp\":40,"
        "\"pd\":31,\"rd\":\"Jul 1\",\"t\":1760000000,\"tf\":12,\"ok\":true}", &d, nullptr));
    TEST_ASSERT_EQUAL_FLOAT(12.5f, d.session_pct);
    TEST_ASSERT_EQUAL_INT(240, d.session_reset_mins);
    TEST_ASSERT_EQUAL_FLOAT(38, d.weekly_pct);
    TEST_ASSERT_EQUAL_INT(1500, d.weekly_reset_mins);
    TEST_ASSERT_EQUAL_STRING("allowed", d.status);
    TEST_ASSERT_TRUE(d.chime);
    TEST_ASSERT_TRUE(d.beep);
    TEST_ASSERT_EQUAL_STRING("Claude needs your permission", d.alert_msg);
    TEST_ASSERT_TRUE(d.alert_clear);
    TEST_ASSERT_TRUE(d.enterprise);
    TEST_ASSERT_EQUAL_INT(40, d.time_pct);
    TEST_ASSERT_EQUAL_INT(31, d.period_days);
    TEST_ASSERT_EQUAL_STRING("Jul 1", d.reset_date);
    TEST_ASSERT_EQUAL_INT64(1760000000L, (int64_t)d.clock_epoch);
    TEST_ASSERT_EQUAL_INT(12, d.clock_fmt);
    TEST_ASSERT_TRUE(d.ok);
}

static void test_fast_clock_fields(void) {
    UsageData d;
    TEST_ASSERT_TRUE(payload_parse("{\"a\":2,\"e\":\"b\",\"es\":4000000000,\"n\":3,\"rp\":1}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(2, d.activity);
    TEST_ASSERT_EQUAL_CHAR('b', d.event);
    TEST_ASSERT_TRUE(d.has_event_seq);
    TEST_ASSERT_EQUAL_UINT32(4000000000UL, d.event_seq);
    TEST_ASSERT_EQUAL_INT(3, d.sessions);
    TEST_ASSERT_TRUE(d.replay);
}

static void test_fast_clock_clamps(void) {
    UsageData d;
    TEST_ASSERT_TRUE(payload_parse("{\"a\":9,\"n\":-4,\"es\":0}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(3, d.activity);
    TEST_ASSERT_EQUAL_INT(0, d.sessions);
    TEST_ASSERT_TRUE(d.has_event_seq);             // 0 is a real seq, not absent
    TEST_ASSERT_TRUE(payload_parse("{\"a\":-1}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(0, d.activity);
}

static void test_long_strings_truncate(void) {
    UsageData d;
    TEST_ASSERT_TRUE(payload_parse("{\"st\":\"abcdefghijklmnopqrstuvwxyz\"}", &d, nullptr));
    TEST_ASSERT_EQUAL_size_t(sizeof(d.status) - 1, strlen(d.status));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_rejects_malformed_json);
    RUN_TEST(test_empty_object_gives_legacy_defaults);
    RUN_TEST(test_legacy_fields);
    RUN_TEST(test_fast_clock_fields);
    RUN_TEST(test_fast_clock_clamps);
    RUN_TEST(test_long_strings_truncate);
    return UNITY_END();
}
