#include <Arduino.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include "display_cfg.h"
#include "data.h"
#include "ui.h"
#include "ble.h"
#include "power.h"
#include "imu.h"
#include "env_sensor.h"
#include "sensor_hist.h"
#include "usage_hist.h"
#include "splash.h"
#include "usage_rate.h"
#include "wifi_net.h"
#include "web_server.h"
#include "device_api.h"

// Physical buttons:
//   BTN_BACK  (GPIO 0 / BOOT) — cycle screens (splash→usage→bluetooth→usage…)
//   NOTE: GPIO18 is LCD SCLK on this board — right button not available
//   NOTE: AXP2101 PWR button not available on this board
#define BTN_BACK 0

// ---- Hardware objects ----
// ST7789 via hardware SPI (VSPI: MOSI=23, SCLK=18)
// col_offset=52, row_offset=40 are standard for 1.14" 135x240 ST7789 panels
Arduino_DataBus *bus = new Arduino_ESP32SPI(
    LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI, GFX_NOT_DEFINED /* MISO */);
Arduino_ST7789  *gfx = new Arduino_ST7789(
    bus, LCD_RESET, 0 /* rotation */, true /* IPS */,
    LCD_WIDTH, LCD_HEIGHT, 52, 40);

static UsageData usage = {};
static CopilotData copilot = {};
static SysInfoData sysinfo = {};
static VscodeData vscode = {};
static EnvData envd = {};
static AuroraData aurorad = {};
static CiData cid = {};
static TodayData todayd = {};

// ---- LVGL draw buffers (partial render) ----
// Was 40 (21.6 KB across both buffers) — trimmed to give WiFi's runtime init
// (esp_wifi_init's own task + RX/TX buffer pools, ~40-70 KB, only allocated
// once WiFi.mode() actually runs) enough free heap to succeed. More flush()
// calls per full redraw (15 vs 6) is imperceptible for this UI's mostly
// text/bar content — not a video buffer.
// Was 40 (21.6 KB across both buffers) — trimmed to give WiFi's runtime init
// (esp_wifi_init's own task + RX/TX buffer pools, ~40-70 KB, only allocated
// once WiFi.mode() actually runs) enough free heap to succeed. More flush()
// calls per full redraw (15 vs 6) is imperceptible for this UI's mostly
// text/bar content — not a video buffer.
#define BUF_LINES 16
static uint16_t *buf1 = nullptr;
static uint16_t *buf2 = nullptr;

// LVGL tick callback
static uint32_t my_tick(void) {
    return millis();
}

// When true, my_flush_cb also captures each flushed tile — either streamed
// over serial (shot_fb == nullptr, the QA `screenshot` command) or copied
// into shot_fb (the HTTP screenshot endpoint, web_server.cpp). Either way no
// full-frame buffer is kept permanently — heap is too fragmented on this
// board for that; the caller mallocs a transient buffer only when needed.
static volatile bool shot_active = false;
static uint16_t*     shot_fb     = nullptr;

// LVGL flush callback — ST7789 direct SPI write, no rotation needed
static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)px_map, w, h);
    if (shot_active) {
        if (shot_fb) {
            const uint16_t* src = (const uint16_t*)px_map;
            for (int32_t row = 0; row < h; row++) {
                memcpy(&shot_fb[(area->y1 + row) * LCD_WIDTH + area->x1],
                       &src[row * w], (size_t)w * 2);
            }
        } else {
            Serial.printf("A %ld %ld %ld %ld\n",
                (long)area->x1, (long)area->y1, (long)area->x2, (long)area->y2);
            Serial.write(px_map, (size_t)(w * h * 2));
            Serial.write('\n');
            Serial.flush();
        }
    }
    lv_display_flush_ready(disp);
}

// Force a full redraw and capture it via my_flush_cb (either sink above).
static void capture_screenshot(void) {
    shot_active = true;
    lv_obj_invalidate(lv_screen_active());
    for (int i = 0; i < 30; i++) {   // ~1.5s ceiling; a full 135x240 redraw is a handful of tiles
        lv_timer_handler();
        delay(5);
    }
    shot_active = false;
}

// Capture the current LVGL frame into `out` (must hold LCD_WIDTH*LCD_HEIGHT
// uint16_t RGB565 pixels). Declared in device_api.h for web_server.cpp.
bool capture_screenshot_rgb565(uint16_t* out) {
    shot_fb = out;
    capture_screenshot();
    shot_fb = nullptr;
    return true;
}

// Parse a JSON line into UsageData or CopilotData based on the "src" field.
// Dispatching is done at the call site in loop().

// Serial command buffer
#define CMD_BUF_SIZE 256
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

static void send_screenshot() {
    const uint32_t w = LCD_WIDTH, h = LCD_HEIGHT;
    Serial.printf("SCREENSHOT_START %lu %lu %lu\n",
        (unsigned long)w, (unsigned long)h, (unsigned long)(w * h * 2));
    Serial.flush();

    capture_screenshot();   // shot_fb is null here, so my_flush_cb streams to Serial

    Serial.println("SCREENSHOT_END");
}

// Parse one JSON payload (from BLE, or the `feed` serial command) and push it
// into the UI. Routed by the "src" field; default is Claude usage.
static bool process_payload(const char* raw) {
    JsonDocument doc;
    if (deserializeJson(doc, raw) != DeserializationError::Ok) {
        Serial.println("JSON parse error");
        return false;
    }
    const char* src = doc["src"] | "claude";
    if (strcmp(src, "status") == 0) {
        ui_update_daemon_state(doc["state"] | "ok");
    } else if (strcmp(src, "copilot") == 0) {
        copilot.premium_pct        = doc["pp"]  | -1;
        copilot.premium_remaining  = doc["pr"]  | -1;
        copilot.premium_total      = doc["pe"]  | -1;
        copilot.premium_reset_mins = doc["prm"] | -1;
        strlcpy(copilot.premium_reset_str, doc["prd"] | "---", sizeof(copilot.premium_reset_str));
        strlcpy(copilot.plan, doc["plan"] | "unknown", sizeof(copilot.plan));
        copilot.enabled = doc["en"] | false;
        copilot.valid   = true;
        ui_update_copilot(&copilot);
    } else if (strcmp(src, "sysinfo") == 0) {
        sysinfo.cpu_pct       = doc["cpu"] | -1;
        sysinfo.cpu_temp      = doc["ct"]  | -1.0f;
        sysinfo.ram_pct       = doc["rp"]  | -1;
        sysinfo.ram_used_gb   = doc["ru"]  | 0.0f;
        sysinfo.ram_total_gb  = doc["rt"]  | 0.0f;
        sysinfo.disk_pct      = doc["dp"]  | -1;
        sysinfo.disk_used_gb  = doc["du"]  | 0.0f;
        sysinfo.disk_total_gb = doc["dt"]  | 0.0f;
        sysinfo.valid = true;
        ui_update_sysinfo(&sysinfo);
    } else if (strcmp(src, "vscode") == 0) {
        vscode.mem_mb      = doc["mm"] | -1;
        vscode.cpu_pct     = doc["vc"] | -1;
        vscode.ext_count   = doc["xe"] | -1;
        vscode.error_count = doc["ec"] | -1;
        strlcpy(vscode.last_error, doc["le"] | "", sizeof(vscode.last_error));
        vscode.valid = true;
        ui_update_vscode(&vscode);
    } else if (strcmp(src, "act") == 0) {
        ui_update_act(doc["st"] | "idle", doc["n"] | 1);
    } else if (strcmp(src, "ci") == 0) {
        strlcpy(cid.state,  doc["state"] | "none", sizeof(cid.state));
        strlcpy(cid.wf,     doc["wf"]    | "",     sizeof(cid.wf));
        strlcpy(cid.branch, doc["br"]    | "",     sizeof(cid.branch));
        cid.age_min  = doc["age"] | -1;
        cid.review   = doc["rev"] | 0;
        cid.changes  = doc["chg"] | 0;
        cid.dirty    = doc["dty"] | -1;
        cid.ahead    = doc["ah"]  | 0;
        cid.behind   = doc["bh"]  | 0;
        cid.conflict = doc["cf"]  | false;
        cid.valid = true;
        ui_update_ci(&cid);
    } else if (strcmp(src, "sum") == 0) {
        todayd.active_min = doc["am"]  | 0;
        todayd.tok_k      = doc["tk"]  | 0;
        todayd.usd        = doc["usd"] | 0;
        todayd.commits    = doc["cm"]  | 0;
        todayd.cp_used    = doc["cp"]  | -1;
        todayd.valid = true;
        ui_update_today(&todayd);
    } else if (strcmp(src, "env") == 0) {
        envd.epoch       = doc["ts"] | 0L;
        envd.tz_off_min  = doc["tz"] | 0;
        envd.temp_c      = doc["tp"] | 0;
        envd.hi_c        = doc["th"] | 0;
        envd.lo_c        = doc["tl"] | 0;
        envd.wcode       = doc["tc"] | -1;
        strlcpy(envd.loc, doc["tn"] | "", sizeof(envd.loc));
        envd.has_weather = doc["tc"].is<int>();
        envd.valid = true;
        sensor_hist_set_time(envd.epoch);
        usage_hist_set_time(envd.epoch);
        ui_update_env(&envd);
    } else if (strcmp(src, "aurora") == 0) {
        aurorad.epoch     = doc["ts"]    | 0L;
        aurorad.pct       = doc["pct"]   | -1;
        aurorad.kp_x10    = doc["kp"]    | -1;
        aurorad.kpmax_x10 = doc["kpmax"] | -1;
        aurorad.cloud_pct = doc["cloud"] | -1;
        aurorad.night     = doc["night"] | 0;
        aurorad.valid = true;
        ui_update_aurora(&aurorad);
    } else if (strcmp(src, "wifi") == 0) {
        wifi_set_credentials(doc["ssid"] | "", doc["pass"] | "");
    } else {
        usage.session_pct        = doc["s"]  | 0.0f;
        usage.session_reset_mins = doc["sr"] | -1;
        usage.weekly_pct         = doc["w"]  | 0.0f;
        usage.weekly_reset_mins  = doc["wr"] | -1;
        strlcpy(usage.status, doc["st"] | "unknown", sizeof(usage.status));
        strlcpy(usage.model, doc["mdl"] | "", sizeof(usage.model));
        usage.ctx_pct = doc["ctx"] | -1;
        usage.ok    = doc["ok"] | false;
        usage.valid = true;
        int g_before = usage_rate_group();
        usage_rate_sample(usage.session_pct);
        usage_hist_sample(usage.session_pct, usage.weekly_pct);
        if (usage_rate_group() != g_before && splash_is_active()) {
            splash_pick_for_current_rate();
        }
        ui_update(&usage);
    }
    return true;
}

// GET /api/state (web_server.cpp) — dump everything the device currently
// holds, from the same structs process_payload() above populates.
void build_state_json(JsonDocument& doc) {
    JsonObject claude = doc["claude"].to<JsonObject>();
    claude["session_pct"]        = usage.session_pct;
    claude["session_reset_mins"] = usage.session_reset_mins;
    claude["weekly_pct"]         = usage.weekly_pct;
    claude["weekly_reset_mins"]  = usage.weekly_reset_mins;
    claude["status"]             = usage.status;
    claude["model"]              = usage.model;
    claude["ctx_pct"]            = usage.ctx_pct;
    claude["valid"]              = usage.valid;

    JsonObject act = doc["activity"].to<JsonObject>();
    act["state"]  = ui_get_act_state_str();
    act["agents"] = ui_get_act_agents();

    JsonObject cp = doc["copilot"].to<JsonObject>();
    cp["premium_pct"]       = copilot.premium_pct;
    cp["premium_remaining"] = copilot.premium_remaining;
    cp["premium_total"]     = copilot.premium_total;
    cp["premium_reset_str"] = copilot.premium_reset_str;
    cp["plan"]               = copilot.plan;
    cp["enabled"]            = copilot.enabled;
    cp["valid"]              = copilot.valid;

    JsonObject si = doc["sysinfo"].to<JsonObject>();
    si["cpu_pct"]       = sysinfo.cpu_pct;
    si["cpu_temp"]      = sysinfo.cpu_temp;
    si["ram_pct"]       = sysinfo.ram_pct;
    si["ram_used_gb"]   = sysinfo.ram_used_gb;
    si["ram_total_gb"]  = sysinfo.ram_total_gb;
    si["disk_pct"]      = sysinfo.disk_pct;
    si["disk_used_gb"]  = sysinfo.disk_used_gb;
    si["disk_total_gb"] = sysinfo.disk_total_gb;
    si["valid"]         = sysinfo.valid;

    JsonObject vs = doc["vscode"].to<JsonObject>();
    vs["mem_mb"]      = vscode.mem_mb;
    vs["cpu_pct"]     = vscode.cpu_pct;
    vs["ext_count"]   = vscode.ext_count;
    vs["error_count"] = vscode.error_count;
    vs["last_error"]  = vscode.last_error;
    vs["valid"]       = vscode.valid;

    JsonObject env = doc["env"].to<JsonObject>();
    env["temp_c"]      = envd.temp_c;
    env["hi_c"]        = envd.hi_c;
    env["lo_c"]        = envd.lo_c;
    env["wcode"]       = envd.wcode;
    env["loc"]         = envd.loc;
    env["has_weather"] = envd.has_weather;
    env["valid"]       = envd.valid;

    JsonObject au = doc["aurora"].to<JsonObject>();
    au["pct"]       = aurorad.pct;
    au["kp_x10"]    = aurorad.kp_x10;
    au["kpmax_x10"] = aurorad.kpmax_x10;
    au["cloud_pct"] = aurorad.cloud_pct;
    au["night"]     = aurorad.night;
    au["valid"]     = aurorad.valid;

    JsonObject ci = doc["ci"].to<JsonObject>();
    ci["state"]    = cid.state;
    ci["wf"]       = cid.wf;
    ci["branch"]   = cid.branch;
    ci["age_min"]  = cid.age_min;
    ci["review"]   = cid.review;
    ci["changes"]  = cid.changes;
    ci["dirty"]    = cid.dirty;
    ci["ahead"]    = cid.ahead;
    ci["behind"]   = cid.behind;
    ci["conflict"] = cid.conflict;
    ci["valid"]    = cid.valid;

    JsonObject today = doc["today"].to<JsonObject>();
    today["active_min"] = todayd.active_min;
    today["tok_k"]       = todayd.tok_k;
    today["usd"]         = todayd.usd;
    today["commits"]     = todayd.commits;
    today["cp_used"]     = todayd.cp_used;
    today["valid"]       = todayd.valid;

    JsonObject conn = doc["connectivity"].to<JsonObject>();
    conn["ble_state"]   = (int)ble_get_state();
    conn["ble_device"]  = ble_get_device_name();
    conn["daemon_state"] = ui_get_daemon_state();
    conn["wifi_state"]  = (int)wifi_get_state();
    conn["wifi_ip"]     = wifi_get_ip();
}

// ---- Backlight: steady / breathe-while-working / idle-dim ----
static uint32_t last_interaction_ms = 0;
#define BL_IDLE_MS      600000UL   // 10 min with no press/data → dim
#define BL_FULL        255
#define BL_IDLE         90         // ~35%
#define BL_BREATHE_LO  150

static void note_interaction(void) { last_interaction_ms = millis(); }

static void backlight_tick(void) {
    static uint32_t last = 0;
    static int cur = BL_FULL;
    uint32_t now = millis();
    if (now - last < 33) return;   // ~30 Hz
    last = now;

    int target;
    if (ui_claude_working()) {
        // ~4 s triangle breathe between BL_BREATHE_LO and BL_FULL — a
        // peripheral "still going" signal that outranks the idle dim.
        uint32_t p = now % 4000;
        uint32_t tri = (p < 2000) ? p : (4000 - p);   // 0..2000..0
        target = BL_BREATHE_LO + (int)((BL_FULL - BL_BREATHE_LO) * tri / 2000);
    } else if (now - last_interaction_ms > BL_IDLE_MS) {
        target = BL_IDLE;   // no button press in 10 min → you've stepped away
    } else {
        target = BL_FULL;
    }
    // ease toward target
    cur += (target - cur) / 6;
    if (cur < BL_IDLE) cur = BL_IDLE;
    if (cur > BL_FULL) cur = BL_FULL;
    ledcWrite(LCD_BLK, cur);
}

static void check_serial_cmd() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            if (strcmp(cmd_buf, "screenshot") == 0) {
                send_screenshot();
            } else if (strncmp(cmd_buf, "screen ", 7) == 0) {
                // QA helper: jump to a screen without the physical button.
                int n = atoi(cmd_buf + 7);
                if (n >= 0 && n < SCREEN_COUNT) {
                    ui_show_screen((screen_t)n);
                    Serial.printf("screen -> %d\n", n);
                }
            } else if (strncmp(cmd_buf, "feed ", 5) == 0) {
                // QA helper: inject a payload as if it arrived over BLE.
                Serial.println(process_payload(cmd_buf + 5) ? "feed ok" : "feed err");
            } else if (strcmp(cmd_buf, "timer") == 0) {
                ui_timer_toggle();
                Serial.println("timer toggled");
            } else if (strcmp(cmd_buf, "i2cscan") == 0) {
                env_sensor_scan_bus();
            } else if (strcmp(cmd_buf, "gpiotest") == 0) {
                env_sensor_gpio_test();
            } else if (strcmp(cmd_buf, "histclear") == 0) {
                sensor_hist_clear();
                Serial.println("sensor history cleared");
            } else if (strcmp(cmd_buf, "histfill") == 0) {
                sensor_hist_debug_fill();   // QA: synthetic 24 h trace
            } else if (strcmp(cmd_buf, "uhistclear") == 0) {
                usage_hist_clear();
                Serial.println("usage history cleared");
            } else if (strcmp(cmd_buf, "uhistfill") == 0) {
                usage_hist_debug_fill();   // QA: synthetic 24 h trace
            }
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("{\"ready\":true}");

    // Init display
    gfx->begin();
    gfx->fillScreen(0x0000);
    // LCD backlight on a PWM channel — steady at full, breathes while Claude
    // is working, dims after a stretch of no interaction (backlight_tick()).
    ledcAttach(LCD_BLK, 20000, 8);
    ledcWrite(LCD_BLK, 255);

    // Init PMU stub (no-op)
    power_init();

    // Init IMU stub (no-op)
    imu_init();

    // Init environmental sensor (I2C, SDA=21/SCL=22) — auto-detects a
    // BME280 or BMP180, whichever is wired up. No-op UI impact if nothing's
    // wired: sensor_has_data stays false and the Sensor screen is skipped
    // in the cycle.
    env_sensor_init();

    // Restore the 24 h sensor trend from NVS. Must follow env_sensor_init()
    // only in spirit (it doesn't touch I2C) but must precede ui_init(), which
    // draws the trend graphs from it.
    sensor_hist_init();
    usage_hist_init();

    // Init LVGL
    lv_init();
    lv_tick_set_cb(my_tick);

    // Allocate partial render buffers (no PSRAM on ESP32-WROOM)
    buf1 = (uint16_t*)malloc(LCD_WIDTH * BUF_LINES * 2);
    buf2 = (uint16_t*)malloc(LCD_WIDTH * BUF_LINES * 2);

    lv_display_t* disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, LCD_WIDTH * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    // Init BLE data channel
    ble_init();

    // Physical button: back (GPIO 0 / BOOT button)
    pinMode(BTN_BACK, INPUT_PULLUP);

    // Build dashboard — deliberately BEFORE wifi_init() below: this makes
    // its one-time mallocs (LVGL partial buffers already done above, the
    // ~29 KB splash canvas here) claim heap while it's most free. WiFi's
    // own runtime buffers (~40-70 KB, allocated once WiFi.mode() actually
    // runs — not visible in the static RAM% the build report prints) are
    // the biggest heap consumer added by this feature; letting the UI go
    // first avoids it starving the splash canvas malloc (which used to
    // crash the device — see the null-guards in splash.cpp — when it lost
    // that race).
    ui_init();

    // Show initial BLE status on Bluetooth screen
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());

    // Show initial battery status
    ui_update_battery(power_battery_pct(), power_is_charging());

    ui_show_screen(SCREEN_SPLASH);

    // WiFi is additive to BLE, not a replacement — connects only if
    // credentials were previously provisioned (over BLE, see process_payload's
    // "wifi" case). web_server_init() only registers routes; it defers
    // actually starting the listener until WiFi has a real IP (see its own
    // comment in web_server.cpp for why that has to be deferred).
    wifi_init();
    web_server_init();

    Serial.println("Dashboard ready, waiting for data on BLE...");
}

static ble_state_t last_ble_state = BLE_STATE_INIT;

void loop() {
    lv_timer_handler();
    ui_tick_anim();
    ble_tick();
    wifi_tick();
    web_server_tick();
    power_tick();
    imu_tick();
    env_sensor_tick();
    sensor_hist_tick();
    usage_hist_tick();
    splash_tick();
    backlight_tick();

    // Single button (GPIO 0 / BOOT) — the only input on this board (no touch):
    //   Short press       → next screen (Usage → Copilot → System → VS Code →
    //                       Bluetooth → Splash → …; unpopulated screens skipped)
    //   Quick double-press → jump straight to the Clock ("home") screen —
    //                       with up to 10 screens in the cycle, waiting on
    //                       every short press to see if a second one follows
    //                       would make normal cycling feel laggy, so instead
    //                       the first press always cycles immediately and a
    //                       second one landing within DOUBLE_PRESS_MS just
    //                       redirects straight to Clock.
    //   Long press        → Bluetooth screen: clear the BLE bond;
    //                       any other screen: ask the daemon for a fresh poll
    //   NOTE: GPIO18 = LCD SCLK (no right button); AXP PWR not present
    {
        static bool     btn_was = false;
        static uint32_t btn_down_ms = 0;
        static bool     long_fired = false;
        static uint32_t last_release_ms = 0;
        const uint32_t  LONG_PRESS_MS = 700;
        const uint32_t  DOUBLE_PRESS_MS = 350;

        bool btn_now = (digitalRead(BTN_BACK) == LOW);

        if (btn_now && !btn_was) {
            btn_down_ms = millis();
            long_fired = false;
            note_interaction();
        } else if (btn_now && btn_was && !long_fired &&
                   millis() - btn_down_ms >= LONG_PRESS_MS) {
            long_fired = true;  // fire once, while still held
            note_interaction();
            screen_t cs = ui_get_current_screen();
            if (cs == SCREEN_BLUETOOTH) {
                ui_flash_feedback_strong();
                ble_clear_bonds();
            } else if (cs == SCREEN_CLOCK) {
                ui_timer_toggle();   // start/stop the focus timer (flashes itself)
            } else {
                ui_flash_feedback_strong();
                ble_request_refresh();
            }
        } else if (!btn_now && btn_was && !long_fired) {
            ui_flash_feedback();  // released before long-press threshold
            if (ui_banner_visible()) {
                ui_hide_banner();  // dismiss, don't advance
            } else {
                uint32_t now = millis();
                bool is_double = (now - last_release_ms) <= DOUBLE_PRESS_MS;
                if (is_double && ui_get_current_screen() != SCREEN_CLOCK) {
                    last_release_ms = 0;  // consumed — a 3rd quick press starts fresh
                    ui_show_screen(SCREEN_CLOCK);
                } else {
                    last_release_ms = now;
                    ui_cycle_screen();
                }
            }
        }
        btn_was = btn_now;
    }

    // Update BLE status on screen when state changes
    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    // Update WiFi status on screen when state changes (IP only settles once
    // CONNECTED, so re-push on every tick while in that state is unnecessary —
    // a state-change edge is enough since the IP doesn't change once assigned).
    static wifi_state_t last_wifi_state = (wifi_state_t)-1;
    wifi_state_t ws = wifi_get_state();
    if (ws != last_wifi_state) {
        last_wifi_state = ws;
        ui_update_wifi_status(ws, wifi_get_ip().c_str(), wifi_get_token());
    }

    // Update battery indicator
    static int last_pct = -2;
    static bool last_charging = false;
    int pct = power_battery_pct();
    bool charging = power_is_charging();
    if (pct != last_pct || charging != last_charging) {
        last_pct = pct;
        last_charging = charging;
        ui_update_battery(pct, charging);
    }

    // Push env sensor reading to the UI on change (sensor is local, not a BLE payload)
    {
        static bool  last_present = false;
        static float last_t = -999, last_p = -999, last_h = -999;
        bool  s_present = env_sensor_is_present();
        float s_t = env_sensor_temp_c(), s_p = env_sensor_pressure_hpa();
        bool  s_has_h = env_sensor_has_humidity();
        float s_h = s_has_h ? env_sensor_humidity_pct() : 0;
        if (s_present != last_present || s_t != last_t || s_p != last_p || s_h != last_h) {
            last_present = s_present; last_t = s_t; last_p = s_p; last_h = s_h;
            ui_update_sensor(s_present, s_t, s_p, s_has_h, s_h);
        }

        // Feed the 24 h history on a fixed cadence rather than on change, so
        // each 15-minute slot averages a consistent number of readings.
        static uint32_t hist_ms = 0;
        if (s_present && (hist_ms == 0 || millis() - hist_ms >= 10000)) {
            hist_ms = millis();
            sensor_hist_sample(s_t, s_has_h, s_h, s_p);
        }
    }

    // Check for serial commands (screenshot, etc.)
    check_serial_cmd();

    // Process incoming BLE data — route by "src" field (default: "claude")
    if (ble_has_data()) {
        if (process_payload(ble_get_data())) ble_send_ack();
        else                                 ble_send_nack();
    }

    delay(5);
}
