#include "payload.h"
#include "button_action.h"
#include <ArduinoJson.h>
#include <string.h>

// strlcpy isn't in every host libc; this is all we need of it.
static void copy_str(char* dst, size_t cap, const char* src) {
    if (!cap) return;
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
}

// A button action (`k1`..`k3`): -1 when absent, not an integer, or a code
// this firmware doesn't know (a newer app), so the stored action stays
// rather than the default being saved over it.
static int32_t button_field(JsonVariant v) {
    if (!v.is<int32_t>()) return -1;
    return button_action_clamp(v.as<int32_t>(), -1);
}

// A count (`rc`): -1 when absent or not a number, else 0..999.
static int16_t clamp_count(JsonVariant v) {
    if (!v.is<int32_t>()) return -1;
    const int32_t n = v.as<int32_t>();
    return (int16_t)(n < 0 ? 0 : n > 999 ? 999 : n);
}

bool payload_parse(const char* json, UsageData* out, const char** err) {
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, json);
    if (e) {
        if (err) *err = e.c_str();
        return false;
    }

    UsageData d = {};
    d.session_pct = doc["s"] | 0.0f;
    d.session_reset_mins = doc["sr"] | -1;
    d.weekly_pct = doc["w"] | 0.0f;
    d.weekly_reset_mins = doc["wr"] | -1;
    // Absent or negative: unknown, never 0 %. Clamped to what the overlay
    // can use; fable_bar_pct() caps it at the limit.
    const float wf = doc["wf"] | -1.0f;
    d.fable_pct = wf < 0 ? -1 : wf >= 999.0f ? 999 : (int16_t)(wf + 0.5f);
    copy_str(d.status, sizeof(d.status), doc["st"] | "unknown");
    // as<bool>, not `| false`: daemons send 1, and `|` only accepts a JSON bool.
    d.chime = doc["c"].as<bool>();   // absent (old daemon / chime off) → stay silent
    d.beep = doc["b"].as<bool>();
    copy_str(d.alert_msg, sizeof(d.alert_msg), doc["m"] | "");
    d.alert_clear = doc["x"].as<bool>();
    const char* acct = doc["acct"] | "pro";
    d.enterprise = (strcmp(acct, "ent") == 0);
    d.weekly_only = (strcmp(acct, "week") == 0);
    if (d.weekly_only) {
        // No 5-hour limit: the app leaves s/sr "not started" for firmware
        // that predates "week"; here the week is the main meter.
        d.session_pct = d.weekly_pct;
        d.session_reset_mins = d.weekly_reset_mins;
    }
    // Codex (a trial): the provider, then its reset credits, each -1 when
    // absent (unknown, never 0).
    d.codex = strcmp(doc["pv"] | "", "codex") == 0;
    if (d.codex) d.fable_pct = -1;   // Fable is Claude's: never a strip on the Codex screen
    d.resets_held = clamp_count(doc["rc"]);
    JsonVariant rx = doc["rx"];
    d.resets_expiry_n = -1;
    if (rx.is<JsonArray>()) {
        d.resets_expiry_n = 0;
        for (JsonVariant v : rx.as<JsonArray>()) {
            if (d.resets_expiry_n == (int8_t)(sizeof(d.resets_expiry) / sizeof(d.resets_expiry[0]))) break;
            if (!v.is<const char*>()) continue;
            copy_str(d.resets_expiry[d.resets_expiry_n++], sizeof(d.resets_expiry[0]), v.as<const char*>());
        }
    }
    copy_str(d.resets_kind, sizeof(d.resets_kind), doc["rf"] | "");
    d.time_pct = doc["tp"] | 0;
    d.period_days = doc["pd"] | 30;
    copy_str(d.reset_date, sizeof(d.reset_date), doc["rd"] | "");
    d.clock_epoch = doc["t"] | 0L;
    d.clock_fmt = doc["tf"] | 24;

    // ---- fast clock (optional) ----
    JsonVariant a = doc["a"];
    if (a.isNull()) {
        d.activity = -1;
    } else {
        int lvl = a | 0;
        d.activity = (int8_t)(lvl < 0 ? 0 : lvl > 3 ? 3 : lvl);
    }
    const char* ev = doc["e"] | "";
    d.event = ev[0];
    JsonVariant es = doc["es"];
    d.has_event_seq = !es.isNull();
    d.event_seq = d.has_event_seq ? es.as<uint32_t>() : 0;
    JsonVariant n = doc["n"];
    if (n.isNull()) {
        d.sessions = -1;
    } else {
        int v = n | 0;
        d.sessions = (int16_t)(v < 0 ? 0 : v > 999 ? 999 : v);
    }
    d.replay = doc["rp"].as<bool>();
    JsonVariant lv = doc["lv"];
    if (lv.isNull()) {
        d.liveliness = -1;
    } else {
        int v = lv | 3;
        d.liveliness = (int8_t)(v < 1 ? 1 : v > 5 ? 5 : v);
    }
    JsonVariant aa = doc["aa"];
    if (aa.isNull()) {
        d.attn_anim_s = -1;
    } else {
        int v = aa | 120;
        d.attn_anim_s = (int16_t)(v < 0 ? 0 : v > 120 ? 120 : v);
    }

    JsonVariant pm = doc["pm"];
    d.pace_marker = pm.isNull() ? -1 : (int8_t)((pm | 0) ? 1 : 0);
    d.btn_primary   = button_field(doc["k1"]);
    d.btn_secondary = button_field(doc["k2"]);
    d.btn_pwr       = button_field(doc["k3"]);
    d.has_theme = theme_parse_hex(doc["th"] | "", d.theme);
    d.has_fable_color = theme_parse_rgb(doc["fc"] | "", &d.fable_color);

    d.ok = doc["ok"] | false;
    d.valid = true;
    *out = d;
    return true;
}
