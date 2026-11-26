// payload_parse: long-standing fields keep their defaults, fast-clock fields
// are optional and clamped.
//   pio test -d firmware -e native -f test_payload
#include <unity.h>
#include <string.h>
#include <stdio.h>
#include "payload.h"
#include "fable.h"

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

static void test_account_kinds(void) {
    UsageData d;
    // No 5-hour limit: s/sr come as "not started"; the week becomes the main meter.
    TEST_ASSERT_TRUE(payload_parse(
        "{\"s\":0,\"sr\":-1,\"w\":48,\"wr\":7900,\"acct\":\"week\",\"ok\":true}", &d, nullptr));
    TEST_ASSERT_TRUE(d.weekly_only);
    TEST_ASSERT_FALSE(d.enterprise);
    TEST_ASSERT_EQUAL_FLOAT(48, d.session_pct);
    TEST_ASSERT_EQUAL_INT(7900, d.session_reset_mins);
    TEST_ASSERT_EQUAL_FLOAT(48, d.weekly_pct);
    TEST_ASSERT_TRUE(payload_parse("{\"acct\":\"pro\"}", &d, nullptr));
    TEST_ASSERT_FALSE(d.weekly_only);
    TEST_ASSERT_TRUE(payload_parse("{\"acct\":\"ent\"}", &d, nullptr));
    TEST_ASSERT_FALSE(d.weekly_only);
    TEST_ASSERT_TRUE(d.enterprise);
    TEST_ASSERT_TRUE(payload_parse("{}", &d, nullptr));
    TEST_ASSERT_FALSE(d.weekly_only);
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

static void test_attention_animation(void) {
    UsageData d;
    TEST_ASSERT_TRUE(payload_parse("{\"s\":1}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(-1, d.attn_anim_s);      // absent: keep the saved setting
    TEST_ASSERT_TRUE(payload_parse("{\"aa\":0}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(0, d.attn_anim_s);       // off
    TEST_ASSERT_TRUE(payload_parse("{\"aa\":30}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(30, d.attn_anim_s);
    TEST_ASSERT_TRUE(payload_parse("{\"aa\":999}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(120, d.attn_anim_s);
}

static void test_liveliness(void) {
    UsageData d;
    TEST_ASSERT_TRUE(payload_parse("{\"ok\":true}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(-1, d.liveliness);       // absent: keep the saved level
    TEST_ASSERT_TRUE(payload_parse("{\"lv\":1}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(1, d.liveliness);
    TEST_ASSERT_TRUE(payload_parse("{\"lv\":9}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(5, d.liveliness);
    TEST_ASSERT_TRUE(payload_parse("{\"lv\":0}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(1, d.liveliness);
}

static void test_pace_marker(void) {
    UsageData d;
    TEST_ASSERT_TRUE(payload_parse("{\"ok\":true}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(-1, d.pace_marker);      // absent: keep the saved setting
    TEST_ASSERT_TRUE(payload_parse("{\"pm\":1}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(1, d.pace_marker);
    TEST_ASSERT_TRUE(payload_parse("{\"pm\":0}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(0, d.pace_marker);
    TEST_ASSERT_TRUE(payload_parse("{\"pm\":7}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(1, d.pace_marker);
}

static void test_screensaver(void) {
    UsageData d;
    TEST_ASSERT_TRUE(payload_parse("{\"ok\":true}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(-1, d.screensaver);      // absent: keep the saved setting
    TEST_ASSERT_EQUAL_INT(-1, d.tap_face);
    TEST_ASSERT_TRUE(payload_parse("{\"ss\":0}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(0, d.screensaver);       // never
    TEST_ASSERT_TRUE(payload_parse("{\"ss\":30}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(30, d.screensaver);
    TEST_ASSERT_TRUE(payload_parse("{\"ss\":999}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(60, d.screensaver);
    TEST_ASSERT_TRUE(payload_parse("{\"pk\":0}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(0, d.tap_face);
    TEST_ASSERT_TRUE(payload_parse("{\"pk\":1}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(1, d.tap_face);
}

static void test_button_actions(void) {
    UsageData d;
    TEST_ASSERT_TRUE(payload_parse("{\"ok\":true}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(-1, d.btn_primary);      // absent: keep the saved action
    TEST_ASSERT_EQUAL_INT(-1, d.btn_secondary);
    TEST_ASSERT_EQUAL_INT(-1, d.btn_pwr);
    TEST_ASSERT_TRUE(payload_parse("{\"k1\":4,\"k2\":0,\"k3\":1}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(4, d.btn_primary);
    TEST_ASSERT_EQUAL_INT(0, d.btn_secondary);
    TEST_ASSERT_EQUAL_INT(1, d.btn_pwr);
    // A Ctrl+Shift+M chord, a held Ctrl+Space, play/pause.
    TEST_ASSERT_TRUE(payload_parse("{\"k1\":66320,\"k2\":69932,\"k3\":131277}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(0x10310, d.btn_primary);
    TEST_ASSERT_EQUAL_INT(0x1112C, d.btn_secondary);
    TEST_ASSERT_EQUAL_INT(0x200CD, d.btn_pwr);
    // Unknown (a newer app) or not an integer: as if absent, so the saved
    // action stays.
    TEST_ASSERT_TRUE(payload_parse("{\"k1\":99,\"k2\":-3,\"k3\":65536}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(-1, d.btn_primary);
    TEST_ASSERT_EQUAL_INT(-1, d.btn_secondary);
    TEST_ASSERT_EQUAL_INT(-1, d.btn_pwr);
    TEST_ASSERT_TRUE(payload_parse("{\"k1\":\"x\",\"k2\":1.5,\"k3\":1e12}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(-1, d.btn_primary);
    TEST_ASSERT_EQUAL_INT(-1, d.btn_secondary);
    TEST_ASSERT_EQUAL_INT(-1, d.btn_pwr);
}

static void test_theme(void) {
    UsageData d;
    TEST_ASSERT_TRUE(payload_parse("{\"ok\":true}", &d, nullptr));
    TEST_ASSERT_FALSE(d.has_theme);                // absent: keep the saved palette
    char hex[THEME_HEX_LEN + 1];
    uint32_t want[TH_COUNT];
    memcpy(want, THEME_DEFAULTS, sizeof(want));
    want[TH_ACCENT] = 0x647d99;
    theme_format_hex(want, hex);
    char json[128];
    snprintf(json, sizeof(json), "{\"th\":\"%s\"}", hex);
    TEST_ASSERT_TRUE(payload_parse(json, &d, nullptr));
    TEST_ASSERT_TRUE(d.has_theme);
    TEST_ASSERT_EQUAL_HEX32_ARRAY(want, d.theme, TH_COUNT);
    // Upper case parses too; the round trip formats lower case.
    for (char* c = hex; *c; c++) if (*c >= 'a' && *c <= 'f') *c -= 32;
    uint32_t got[TH_COUNT];
    TEST_ASSERT_TRUE(theme_parse_hex(hex, got));
    TEST_ASSERT_EQUAL_HEX32_ARRAY(want, got, TH_COUNT);
    // Too short, too long or not hex: ignored, never a half-applied palette.
    TEST_ASSERT_TRUE(payload_parse("{\"th\":\"647d99\"}", &d, nullptr));
    TEST_ASSERT_FALSE(d.has_theme);
    hex[5] = 'g';
    TEST_ASSERT_FALSE(theme_parse_hex(hex, got));
    TEST_ASSERT_EQUAL_HEX32_ARRAY(want, got, TH_COUNT);   // untouched
    TEST_ASSERT_TRUE(payload_parse("{\"th\":5}", &d, nullptr));
    TEST_ASSERT_FALSE(d.has_theme);
}

// `wf`: Fable's weekly %, unknown (-1) unless sent; fable_bar_pct() maps it
// onto the weekly bar at half its width.
static void test_fable_weekly(void) {
    UsageData d;
    TEST_ASSERT_TRUE(payload_parse("{\"w\":60,\"ok\":true}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(-1, d.fable_pct);
    TEST_ASSERT_TRUE(payload_parse("{\"w\":60,\"wf\":0,\"ok\":true}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(0, d.fable_pct);
    TEST_ASSERT_TRUE(payload_parse("{\"w\":60,\"wf\":40,\"ok\":true}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(40, d.fable_pct);
    TEST_ASSERT_TRUE(payload_parse("{\"wf\":-1}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(-1, d.fable_pct);
    TEST_ASSERT_TRUE(payload_parse("{\"wf\":\"x\"}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(-1, d.fable_pct);
    TEST_ASSERT_TRUE(payload_parse("{\"wf\":12345}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(999, d.fable_pct);
    // The weekly-only meter keeps it alongside the week it moved up.
    TEST_ASSERT_TRUE(payload_parse("{\"w\":48,\"wf\":3,\"acct\":\"week\",\"ok\":true}", &d, nullptr));
    TEST_ASSERT_TRUE(d.weekly_only);
    TEST_ASSERT_EQUAL_INT(3, d.fable_pct);

    // `fc`: the overlay's colour, its own key beside `th`.
    TEST_ASSERT_FALSE(d.has_fable_color);
    TEST_ASSERT_TRUE(payload_parse("{\"fc\":\"bd93f9\"}", &d, nullptr));
    TEST_ASSERT_TRUE(d.has_fable_color);
    TEST_ASSERT_EQUAL_HEX32(0xbd93f9, d.fable_color);
    TEST_ASSERT_TRUE(payload_parse("{\"fc\":\"#bd93f9\"}", &d, nullptr));
    TEST_ASSERT_FALSE(d.has_fable_color);

    TEST_ASSERT_EQUAL_FLOAT(-1.0f, fable_bar_pct(-1));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, fable_bar_pct(0));
    TEST_ASSERT_EQUAL_FLOAT(20.0f, fable_bar_pct(40));
    TEST_ASSERT_EQUAL_FLOAT(50.0f, fable_bar_pct(100));
    TEST_ASSERT_EQUAL_FLOAT(50.0f, fable_bar_pct(250));   // past the limit: still halfway

    // On a 400 px bar with the week at 60 % (lv_bar's fill: 240 px), the
    // overlay is 80 px at Fable 40 %, half the bar at 100 %, and never past
    // the fill.
    TEST_ASSERT_EQUAL_INT(240, fable_fill_w(400, 60));
    TEST_ASSERT_EQUAL_INT(0, fable_fill_w(400, -3));
    TEST_ASSERT_EQUAL_INT(400, fable_fill_w(400, 140));
    TEST_ASSERT_EQUAL_INT(0, fable_overlay_w(-1, 60, 400, 240));
    TEST_ASSERT_EQUAL_INT(0, fable_overlay_w(0, 60, 400, 240));
    TEST_ASSERT_EQUAL_INT(80, fable_overlay_w(40, 60, 400, 240));
    TEST_ASSERT_EQUAL_INT(200, fable_overlay_w(100, 60, 400, 240));
    TEST_ASSERT_EQUAL_INT(120, fable_overlay_w(100, 30, 400, fable_fill_w(400, 30)));   // clamped to the fill
    TEST_ASSERT_EQUAL_INT(0, fable_overlay_w(40, 0, 400, 0));
    // Fable is part of the week: Fable's share within 1.5 points of the
    // fill's end (wf and w rounded apart) runs to it; a real gap stays.
    TEST_ASSERT_EQUAL_INT(124, fable_overlay_w(61, 31, 400, fable_fill_w(400, 31)));
    TEST_ASSERT_EQUAL_INT(120, fable_overlay_w(60, 30, 400, fable_fill_w(400, 30)));
    TEST_ASSERT_EQUAL_INT(80, fable_overlay_w(40, 60, 400, fable_fill_w(400, 60)));
    // Inclusive 1-point cases, and the 1.5-point boundary either side.
    TEST_ASSERT_TRUE(fable_reaches_fill(58, 30));
    TEST_ASSERT_TRUE(fable_reaches_fill(78, 40));
    TEST_ASSERT_TRUE(fable_reaches_fill(96, 49));
    TEST_ASSERT_TRUE(fable_reaches_fill(12, 7));
    TEST_ASSERT_TRUE(fable_reaches_fill(77, 40));    // 38.5 of 40: exactly 1.5 short
    TEST_ASSERT_FALSE(fable_reaches_fill(76, 40));   // 38 of 40: 2 short
    TEST_ASSERT_FALSE(fable_reaches_fill(70, 40));   // 35 of 40: other models' 5 points stay
    TEST_ASSERT_TRUE(fable_reaches_fill(250, 50));   // past the limit: still half, at the fill's end
    // The same in points on any bar: the M5StickS3's 111 px bar at 40/70
    // keeps its gap.
    TEST_ASSERT_EQUAL_INT(39, fable_overlay_w(70, 40, 111, fable_fill_w(111, 40)));
}

static void test_long_strings_truncate(void) {
    UsageData d;
    TEST_ASSERT_TRUE(payload_parse("{\"st\":\"abcdefghijklmnopqrstuvwxyz\"}", &d, nullptr));
    TEST_ASSERT_EQUAL_size_t(sizeof(d.status) - 1, strlen(d.status));
}

// Codex (a trial): `pv` names the provider, `rc` the reset credits held and
// `rx` their expiry dates. Absent is -1 (unknown), never 0; the count clamps
// to 0..999.
static void test_codex_fields(void) {
    UsageData d;
    TEST_ASSERT_TRUE(payload_parse("{\"s\":10,\"ok\":true}", &d, nullptr));
    TEST_ASSERT_FALSE(d.codex);
    TEST_ASSERT_EQUAL_INT(-1, d.resets_held);
    TEST_ASSERT_TRUE(payload_parse("{\"s\":0,\"sr\":-1,\"w\":34,\"wr\":8478,\"st\":\"allowed\",\"acct\":\"week\","
                                   "\"pv\":\"codex\",\"rc\":3,\"ok\":true}", &d, nullptr));
    TEST_ASSERT_TRUE(d.codex);
    TEST_ASSERT_TRUE(d.weekly_only);
    TEST_ASSERT_EQUAL_INT(-1, d.fable_pct);
    // Fable is Claude's: a Claude payload's strip, then a Codex one that
    // (wrongly) carries `wf`, still has no Fable on the Codex screen.
    TEST_ASSERT_TRUE(payload_parse("{\"w\":60,\"wf\":40,\"acct\":\"pro\",\"ok\":true}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(40, d.fable_pct);
    TEST_ASSERT_TRUE(payload_parse("{\"w\":34,\"wf\":40,\"acct\":\"week\",\"pv\":\"codex\",\"ok\":true}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(-1, d.fable_pct);
    TEST_ASSERT_TRUE(payload_parse("{\"s\":0,\"sr\":-1,\"w\":34,\"wr\":8478,\"st\":\"allowed\",\"acct\":\"week\","
                                   "\"pv\":\"codex\",\"rc\":3,\"ok\":true}", &d, nullptr));
    TEST_ASSERT_EQUAL_FLOAT(34, d.session_pct);   // the week is the main meter
    TEST_ASSERT_EQUAL_INT(3, d.resets_held);
    TEST_ASSERT_EQUAL_INT(-1, d.resets_expiry_n);
    TEST_ASSERT_TRUE(payload_parse("{\"pv\":\"codex\",\"rc\":2,\"rx\":[\"Nov 27\",7,\"Dec 10\"]}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(2, d.resets_expiry_n);   // a non-string is skipped
    TEST_ASSERT_EQUAL_STRING("Nov 27", d.resets_expiry[0]);
    TEST_ASSERT_EQUAL_STRING("Dec 10", d.resets_expiry[1]);
    TEST_ASSERT_TRUE(payload_parse("{\"rx\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\",\"j\",\"k\",\"l\"]}",
                                   &d, nullptr));
    TEST_ASSERT_EQUAL_INT(10, d.resets_expiry_n);   // capped
    TEST_ASSERT_EQUAL_STRING("j", d.resets_expiry[9]);
    TEST_ASSERT_TRUE(payload_parse("{\"rx\":[\"a very long date\"]}", &d, nullptr));
    TEST_ASSERT_EQUAL_STRING("a very ", d.resets_expiry[0]);   // truncated to fit
    TEST_ASSERT_TRUE(payload_parse("{\"pv\":\"codex\",\"rc\":-3}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(0, d.resets_held);
    TEST_ASSERT_TRUE(payload_parse("{\"pv\":\"codex\",\"rc\":5000}", &d, nullptr));
    TEST_ASSERT_EQUAL_INT(999, d.resets_held);
    TEST_ASSERT_TRUE(payload_parse("{\"pv\":\"other\",\"rc\":\"x\"}", &d, nullptr));
    TEST_ASSERT_FALSE(d.codex);
    TEST_ASSERT_EQUAL_INT(-1, d.resets_held);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_codex_fields);
    RUN_TEST(test_rejects_malformed_json);
    RUN_TEST(test_empty_object_gives_legacy_defaults);
    RUN_TEST(test_legacy_fields);
    RUN_TEST(test_account_kinds);
    RUN_TEST(test_fast_clock_fields);
    RUN_TEST(test_fast_clock_clamps);
    RUN_TEST(test_liveliness);
    RUN_TEST(test_attention_animation);
    RUN_TEST(test_pace_marker);
    RUN_TEST(test_screensaver);
    RUN_TEST(test_button_actions);
    RUN_TEST(test_theme);
    RUN_TEST(test_fable_weekly);
    RUN_TEST(test_long_strings_truncate);
    return UNITY_END();
}
