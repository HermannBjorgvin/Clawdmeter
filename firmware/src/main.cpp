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
static CiData cid = {};

// ---- LVGL draw buffers (partial render) ----
#define BUF_LINES 40
static uint16_t *buf1 = nullptr;
static uint16_t *buf2 = nullptr;

// LVGL tick callback
static uint32_t my_tick(void) {
    return millis();
}

// When true, my_flush_cb also streams each flushed tile over serial so the host
// can reassemble a screenshot — no full-frame buffer needed (heap is too
// fragmented on this board to malloc one; see send_screenshot()).
static volatile bool shot_active = false;

// LVGL flush callback — ST7789 direct SPI write, no rotation needed
static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)px_map, w, h);
    if (shot_active) {
        Serial.printf("A %ld %ld %ld %ld\n",
            (long)area->x1, (long)area->y1, (long)area->x2, (long)area->y2);
        Serial.write(px_map, (size_t)(w * h * 2));
        Serial.write('\n');
        Serial.flush();
    }
    lv_display_flush_ready(disp);
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

    // Force a full redraw; my_flush_cb streams each tile while shot_active.
    shot_active = true;
    lv_obj_invalidate(lv_screen_active());
    for (int i = 0; i < 30; i++) {   // ~1.5s ceiling; a full 135x240 redraw is a handful of tiles
        lv_timer_handler();
        delay(5);
    }
    shot_active = false;

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
        ui_update_today(doc["am"] | 0, doc["tk"] | 0, doc["usd"] | 0,
                        doc["cm"] | 0, doc["cp"] | -1);
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

    // Build dashboard
    ui_init();

    // Show initial BLE status on Bluetooth screen
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());

    // Show initial battery status
    ui_update_battery(power_battery_pct(), power_is_charging());

    ui_show_screen(SCREEN_SPLASH);

    Serial.println("Dashboard ready, waiting for data on BLE...");
}

static ble_state_t last_ble_state = BLE_STATE_INIT;

void loop() {
    lv_timer_handler();
    ui_tick_anim();
    ble_tick();
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
