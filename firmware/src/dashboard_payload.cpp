#include "dashboard_payload.h"

#include <ArduinoJson.h>
#include <string.h>

uint8_t parse_dashboard_json(const char* json, UsageData* out) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return DASHBOARD_UPDATE_NONE;

    uint8_t mask = DASHBOARD_UPDATE_NONE;
    if (!doc["s"].isNull()) {
        out->session_pct = doc["s"] | 0.0f;
        out->session_reset_mins = doc["sr"] | -1;
        out->weekly_pct = doc["w"] | 0.0f;
        out->weekly_reset_mins = doc["wr"] | -1;
        strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
        out->chime = doc["c"] | false;
        const char* acct = doc["acct"] | "pro";
        out->enterprise = strcmp(acct, "ent") == 0;
        out->time_pct = doc["tp"] | 0;
        out->period_days = doc["pd"] | 30;
        strlcpy(out->reset_date, doc["rd"] | "", sizeof(out->reset_date));
        out->clock_epoch = doc["t"] | 0L;
        out->clock_fmt = doc["tf"] | 24;
        out->ok = doc["ok"] | false;
        out->valid = true;
        mask |= DASHBOARD_UPDATE_CLAUDE;
    }

    if (doc.containsKey("x")) {
        mask |= DASHBOARD_UPDATE_CODEX;
        out->codex = {};
        JsonObject codex = doc["x"].as<JsonObject>();
        if (!codex.isNull()) {
            for (JsonObject limit : codex["l"].as<JsonArray>()) {
                if (out->codex.limit_count >= 2) break;
                CodexLimitData& target = out->codex.limits[out->codex.limit_count++];
                target.percent = limit["p"] | 0.0f;
                target.window_mins = limit["wm"] | 0;
                target.reset_mins = limit["rm"] | -1;
            }
            out->codex.tokens_today = codex["td"] | 0U;
            strlcpy(out->codex.plan, codex["pl"] | "", sizeof(out->codex.plan));
            out->codex.valid = true;
        }
    }

    if (doc.containsKey("a")) {
        JsonObject activity = doc["a"].as<JsonObject>();
        if (!activity.isNull()) {
            if (activity.containsKey("cl")) {
                out->activity.claude_valid = false;
                JsonObject claude = activity["cl"].as<JsonObject>();
                if (!claude.isNull()) {
                    out->activity.claude_open = claude["o"] | 0;
                    out->activity.claude_busy = claude["b"] | 0;
                    out->activity.claude_waiting = claude["w"] | 0;
                    out->activity.claude_valid = true;
                }
            }
            if (activity.containsKey("cx")) {
                out->activity.codex_valid = false;
                JsonObject codex_activity = activity["cx"].as<JsonObject>();
                if (!codex_activity.isNull()) {
                    out->activity.codex_unread = codex_activity["u"] | 0;
                    out->activity.codex_valid = true;
                }
            }
            out->activity.scanned_epoch = activity["ts"] | 0L;
            out->activity.valid = out->activity.claude_valid || out->activity.codex_valid;
            mask |= DASHBOARD_UPDATE_ACTIVITY;
        }
    }
    out->updated_epoch = doc["ts"] | out->updated_epoch;
    return mask;
}
