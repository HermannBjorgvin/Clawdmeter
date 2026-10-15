#include "payload.h"
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
    copy_str(d.status, sizeof(d.status), doc["st"] | "unknown");
    // as<bool>, not `| false`: daemons send 1, and `|` only accepts a JSON bool.
    d.chime = doc["c"].as<bool>();   // absent (old daemon / chime off) → stay silent
    d.beep = doc["b"].as<bool>();
    copy_str(d.alert_msg, sizeof(d.alert_msg), doc["m"] | "");
    d.alert_clear = doc["x"].as<bool>();
    const char* acct = doc["acct"] | "pro";
    d.enterprise = (strcmp(acct, "ent") == 0);
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

    d.ok = doc["ok"] | false;
    d.valid = true;
    *out = d;
    return true;
}
