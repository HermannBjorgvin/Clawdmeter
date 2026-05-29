// Clawdmeter 3.49 firmware main loop.
// Built on top of Waveshare's 09_LVGL_V8_Test scaffolding (kept verbatim for
// the display init path). Adds NimBLE peripheral for the macOS daemon and
// pipes the parsed payload into the Usage screen.

#include <Arduino.h>
#include <ArduinoJson.h>
#include "user_config.h"
#include "lvgl_port.h"
#include "clawdmeter_ui.h"
#include "ble.h"
#include "battery.h"
#include "esp_err.h"
#include "i2c_bsp.h"
#include "src/lcd_bl_bsp/lcd_bl_pwm_bsp.h"

// Cache the latest daemon values so the periodic battery refresh can call
// clawdmeter_apply_values without overwriting the BLE-sourced numbers with
// stale defaults.
struct LastUsage {
    int sess_pct = 0, week_pct = 0, sessions_count = 0;
    int sess_reset_mins = -1, week_reset_mins = -1;
    long day_c = -1, week_c = -1, month_c = -1, proj_c = -1, burn_c = -1;
    bool have = false;
};
static LastUsage g_last;

// tk.<window> arrays are 8 ints: [in_t, in_c, out_t, out_c, cr_t, cr_c, cw_t, cw_c]
// Sum the odd indices (cost cents from each bucket) for the total.
static long sum_costs(JsonArrayConst arr) {
    if (arr.isNull()) return -1;
    long total = 0;
    for (int i = 1; i < 8; i += 2) total += arr[i].as<long>();
    return total;
}

// Fill out a 4-element tokens-and-cents pair from one window's 8-int array.
static void parse_window(JsonArrayConst arr, long long tok[4], long cents[4]) {
    for (int i = 0; i < 4; i++) { tok[i] = -1; cents[i] = -1; }
    if (arr.isNull()) return;
    for (int b = 0; b < 4; b++) {
        tok[b]   = arr[b * 2].as<long long>();
        cents[b] = arr[b * 2 + 1].as<long>();
    }
}

static void apply_daemon_payload(const char *json) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return;
    }
    int sess_pct = (int)(doc["s"] | 0.0f);
    int week_pct = (int)(doc["w"] | 0.0f);
    int sess_reset_mins = doc["sr"] | -1;
    int week_reset_mins = doc["wr"] | -1;
    JsonArrayConst sess_arr = doc["sess"].as<JsonArrayConst>();
    int sessions_count = sess_arr.isNull() ? 0 : (int)sess_arr.size();

    long day_c = -1, week_c = -1, month_c = -1, proj_c = -1, burn_c = -1;
    long long tok_d[4], tok_w[4], tok_m[4];
    long      cnt_d[4], cnt_w[4], cnt_m[4];
    JsonObjectConst tk = doc["tk"].as<JsonObjectConst>();
    if (!tk.isNull()) {
        parse_window(tk["d"].as<JsonArrayConst>(), tok_d, cnt_d);
        parse_window(tk["w"].as<JsonArrayConst>(), tok_w, cnt_w);
        parse_window(tk["m"].as<JsonArrayConst>(), tok_m, cnt_m);
        day_c   = sum_costs(tk["d"].as<JsonArrayConst>());
        week_c  = sum_costs(tk["w"].as<JsonArrayConst>());
        month_c = sum_costs(tk["m"].as<JsonArrayConst>());
        proj_c  = tk["pj"] | -1L;
        burn_c  = tk["br"] | -1L;
        clawdmeter_apply_tokens(tok_d, cnt_d, tok_w, cnt_w, tok_m, cnt_m);
    }

    // Sessions list - up to 6 entries
    clawdmeter_session_t sessions[6];
    int n = sessions_count > 6 ? 6 : sessions_count;
    for (int i = 0; i < n; i++) {
        JsonObjectConst s = sess_arr[i].as<JsonObjectConst>();
        const char *proj = s["p"] | "session";
        const char *sc   = s["s"] | "i";
        const char *msg  = s["m"] | "";
        snprintf(sessions[i].proj, sizeof(sessions[i].proj), "%s", proj);
        snprintf(sessions[i].msg,  sizeof(sessions[i].msg),  "%s", msg);
        sessions[i].state = (sc[0] == 'w') ? SESS_WAITING_S
                          : (sc[0] == 'k') ? SESS_WORKING_S
                                           : SESS_IDLE_S;
    }
    clawdmeter_apply_sessions(sessions, n);

    // Cache so the battery poll can re-push without stale defaults.
    g_last.sess_pct = sess_pct;
    g_last.week_pct = week_pct;
    g_last.sess_reset_mins = sess_reset_mins;
    g_last.week_reset_mins = week_reset_mins;
    g_last.sessions_count  = sessions_count;
    g_last.day_c = day_c; g_last.week_c = week_c; g_last.month_c = month_c;
    g_last.proj_c = proj_c; g_last.burn_c = burn_c;
    g_last.have = true;

    int batt = 75, chg = 0;
    battery_sample(&batt, &chg);
    clawdmeter_apply_values(sess_pct, week_pct, sess_reset_mins, week_reset_mins,
                            batt, chg,
                            day_c, week_c, month_c, proj_c, burn_c,
                            sessions_count);

    const char *attn = doc["at"] | "";
    clawdmeter_set_attention(attn);
    if (attn[0]) clawdmeter_set_status(attn);
    else         clawdmeter_set_status("BLE: connected");
}

void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println("[clawdmeter] boot");

    i2c_master_Init();
    battery_init();
    lvgl_port_init();
    lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);

    ble_init();
    Serial.printf("[clawdmeter] BLE up, MAC=%s\n", ble_get_mac_address());
}

void loop()
{
    ble_tick();

    if (ble_has_data()) {
        const char *json = ble_get_data();
        Serial.printf("[clawdmeter] BLE rx (%u bytes)\n", (unsigned)strlen(json));
        apply_daemon_payload(json);
        ble_send_ack();
    }

    // Reflect connection state in the footer + Settings tile.
    static ble_state_t last = BLE_STATE_INIT;
    ble_state_t bs = ble_get_state();
    if (bs != last) {
        last = bs;
        const char *footer = "BLE: ?";
        const char *state  = "?";
        switch (bs) {
            case BLE_STATE_ADVERTISING:
                footer = "BLE: advertising - waiting for daemon";
                state  = "advertising";
                break;
            case BLE_STATE_CONNECTED:
                footer = "BLE: connected";
                state  = "connected";
                break;
            case BLE_STATE_DISCONNECTED:
                footer = "BLE: disconnected - re-advertising";
                state  = "disconnected";
                break;
            default: break;
        }
        clawdmeter_set_status(footer);
        clawdmeter_settings_set_ble(state, ble_get_mac_address());
    }

    // Periodic battery refresh (every 5s) - re-pushes cached usage with
    // updated battery so the icon tracks SoC + charging without waiting on
    // the next daemon payload.
    static uint32_t last_batt_ms = 0;
    uint32_t now = millis();
    if (now - last_batt_ms > 5000 && g_last.have) {
        last_batt_ms = now;
        int batt = 75, chg = 0;
        battery_sample(&batt, &chg);
        clawdmeter_apply_values(g_last.sess_pct, g_last.week_pct,
                                g_last.sess_reset_mins, g_last.week_reset_mins,
                                batt, chg,
                                g_last.day_c, g_last.week_c, g_last.month_c,
                                g_last.proj_c, g_last.burn_c,
                                g_last.sessions_count);
    }

    delay(10);
}
