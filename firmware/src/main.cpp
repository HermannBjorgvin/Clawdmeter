#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include "data.h"
#include "ui.h"
#include "theme.h"
#include "ble.h"
#include "splash.h"
#include "usage_rate.h"
#include "idle.h"
#include "idle_cfg.h"
#include "brightness.h"

#include "hal/board_caps.h"
#include "hal/display_hal.h"
#include "hal/touch_hal.h"
#include "hal/input_hal.h"
#include "hal/power_hal.h"
#include "hal/imu_hal.h"
#include "hal/sound_hal.h"

// One slot per provider. The daemon sends every provider it can read in a
// single payload, so switching mode is instant -- no round trip to the host,
// and no stale screen while the next poll lands.
static UsageData usage[THEME_MODE_COUNT] = {};
static inline UsageData* active_usage(void) { return &usage[theme_mode()]; }

// ---- LVGL draw buffers (partial render mode) ----
// PSRAM-equipped boards (S3) can comfortably hold larger strips. PSRAM-free
// boards (e.g. ESP32-C6) allocate from internal SRAM, so we shrink the strip
// — 480×20 RGB565 = 19 KB × 2 buffers = 38 KB, fits beside everything else.
#ifdef BOARD_HAS_PSRAM
#define BUF_LINES 40
#define LV_BUF_CAPS (MALLOC_CAP_SPIRAM)
#else
#define BUF_LINES 20
#define LV_BUF_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif
static uint16_t* buf1 = nullptr;
static uint16_t* buf2 = nullptr;

static uint32_t my_tick(void) { return millis(); }

static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    display_hal_draw_bitmap(area->x1, area->y1, w, h, (uint16_t*)px_map);
    lv_display_flush_ready(disp);
}

static void rounder_cb(lv_event_t* e) {
    lv_area_t* area = (lv_area_t*)lv_event_get_param(e);
    display_hal_round_area(&area->x1, &area->y1, &area->x2, &area->y2);
}

// Touch policy is driven by IDLE_WAKE_ON_TOUCH:
//   true  → a press edge while asleep wakes the device and the first touch is
//           swallowed (mirrors the button wake-consumption); a press while
//           awake counts as activity.
//   false → touch never counts as activity and is fully swallowed while the
//           panel is dark, so pets/sleeves can't wake it overnight and LVGL
//           can't quietly toggle splash<->usage on a black panel.
static void my_touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    uint16_t x, y;
    bool pressed;
    touch_hal_read(&x, &y, &pressed);
    const bool raw_pressed = pressed;

    if (IDLE_WAKE_ON_TOUCH) {
        static bool touch_was = false;
        static bool touch_wake_swallowed = false;
        if (raw_pressed && !touch_was) {
            // Press edge — consume as wake if asleep.
            if (idle_consume_wake_press()) {
                touch_wake_swallowed = true;
                pressed = false;
            }
        } else if (!raw_pressed && touch_was) {
            // Release edge.
            if (touch_wake_swallowed) {
                touch_wake_swallowed = false;
                pressed = false;
            }
        } else if (raw_pressed && touch_wake_swallowed) {
            // Held finger through wake — keep hiding until release.
            pressed = false;
        }
        touch_was = raw_pressed;
    } else if (idle_is_asleep()) {
        pressed = false;
    }

    if (pressed) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// Fill one provider's slot from a JSON object holding the usage fields.
static void parse_provider(JsonObjectConst doc, UsageData* out) {
    out->session_pct = doc["s"] | 0.0f;
    out->session_reset_mins = doc["sr"] | -1;
    out->weekly_pct = doc["w"] | 0.0f;
    out->weekly_reset_mins = doc["wr"] | -1;
    // Weekly scoped-model limits. Absent key (no scoped limits / old daemon)
    // → count 0 and the Weekly card never flips; 0% is a real value.
    out->scoped_weekly_count = 0;
    for (JsonObjectConst lim : doc["ws"].as<JsonArrayConst>()) {
        if (out->scoped_weekly_count >= MAX_SCOPED_WEEKLY) break;
        const char* n = lim["n"] | "";
        if (!n[0]) continue;
        ScopedWeekly& s = out->scoped_weekly[out->scoped_weekly_count++];
        strlcpy(s.name, n, sizeof(s.name));
        s.pct = lim["p"] | 0.0f;
    }
    strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
    out->chime = doc["c"] | false;   // absent (old daemon / chime off) → stay silent
    const char* acct = doc["acct"] | "pro";
    out->enterprise = (strcmp(acct, "ent") == 0);
    out->time_pct = doc["tp"] | 0;
    out->period_days = doc["pd"] | 30;
    strlcpy(out->reset_date, doc["rd"] | "", sizeof(out->reset_date));
    out->clock_epoch = doc["t"] | 0L;
    out->clock_fmt = doc["tf"] | 24;
    // Default true: a daemon that does not send these meters both windows.
    out->has_session = doc["has_s"] | true;
    out->has_weekly  = doc["has_w"] | true;
    strlcpy(out->session_model, doc["sm"] | "", sizeof(out->session_model));
    strlcpy(out->weekly_model,  doc["wm"] | "", sizeof(out->weekly_model));
    out->reset_credits = doc["rc"] | 0;
    out->reset_credits_used = doc["ru"] | 0;
    out->reset_credits_exp_mins = doc["rm"] | -1;
    out->ok = doc["ok"] | false;
    out->valid = true;
}

// Parse a JSON line into the per-provider slots.
//
// Claude's fields sit at the top level and a second provider, when the daemon
// can read one, arrives nested under "x". Keeping Claude flat means an older
// daemon's payload still parses exactly as before -- it simply carries no "x",
// and Codex mode reports no data rather than the screen breaking.
static bool parse_json(const char* json) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }

    parse_provider(doc.as<JsonObjectConst>(), &usage[THEME_MODE_CLAUDE]);

    JsonObjectConst x = doc["x"];
    if (!x.isNull()) {
        parse_provider(x, &usage[THEME_MODE_CODEX]);
        // Clock and chime are host settings, not provider data, so they are
        // sent once at the top level and shared by every slot.
        usage[THEME_MODE_CODEX].chime       = usage[THEME_MODE_CLAUDE].chime;
        usage[THEME_MODE_CODEX].clock_epoch = usage[THEME_MODE_CLAUDE].clock_epoch;
        usage[THEME_MODE_CODEX].clock_fmt   = usage[THEME_MODE_CLAUDE].clock_fmt;
    } else {
        usage[THEME_MODE_CODEX].valid = false;
    }
    return true;
}

// Hold on SECONDARY that means "change how flipping works" rather than "flip
// now". Long enough not to trip on a firm tap, short enough not to feel stuck.
#define MODE_HOLD_MS 600

// Contact bounce settle time. Short enough to feel instant, long enough that a
// noisy edge cannot register as a press.
#define SECONDARY_DEBOUNCE_MS 30

static uint32_t autoflip_last_ms = 0;

// Two flips in quick succession leave the screen where it started, so they
// read as a glitch rather than an action. Observed intermittently on hardware
// at the same millisecond, with the timer measurably correct (60002ms elapsed,
// last-fired updated) -- so the second call comes from somewhere else, most
// likely a dirty edge on SECONDARY that outlasts the debounce. Rather than
// guess further, make a repeat impossible at the one place every flip goes
// through. 250ms is past any contact bounce and short of a deliberate
// double-tap.
#define MIN_FLIP_GAP_MS 250

static void cycle_provider(void) {
    static uint32_t last_flip_ms = 0;
    const uint32_t now = millis();
    if (last_flip_ms && now - last_flip_ms < MIN_FLIP_GAP_MS) return;
    last_flip_ms = now;

    theme_set_mode(theme_next_mode());
    splash_reload_art();
    ui_apply_theme();
    ui_update(active_usage());
    // A manual flip restarts the clock, so the next automatic one is a full
    // interval away rather than possibly landing a second later.
    autoflip_last_ms = millis();
    Serial.printf("mode: -> %s\n", theme().name);
}

// ---- Serial command buffer ----
#define CMD_BUF_SIZE 64
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

static void send_screenshot() {
#ifndef BOARD_HAS_PSRAM
    // A full RGB565 framebuffer doesn't fit in internal SRAM on PSRAM-free
    // boards (e.g. 480×480×2 = 460 KB). Capture is unsupported there.
    Serial.println("SCREENSHOT_UNSUPPORTED");
    return;
#else
    const uint32_t w = board_caps().width;
    const uint32_t h = board_caps().height;
    const uint32_t row_bytes = w * 2;
    const uint32_t buf_size = row_bytes * h;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!sbuf) {
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, row_bytes, sbuf, buf_size);

    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK) {
        heap_caps_free(sbuf);
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    Serial.printf("SCREENSHOT_START %lu %lu %lu\n",
        (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");
    heap_caps_free(sbuf);
#endif
}

static void check_serial_cmd() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            if (strcmp(cmd_buf, "screenshot") == 0) send_screenshot();
            else if (strcmp(cmd_buf, "buzz") == 0)  sound_hal_play_reset();
            // Screen and mode over serial so a UI change can be verified on
            // real hardware without a reflash. The documented workaround was
            // to edit the default boot screen and flash again, which is a
            // 30-second round trip per look and cannot reach the mode at all.
            else if (strcmp(cmd_buf, "usage") == 0)  ui_show_screen(SCREEN_USAGE);
            else if (strcmp(cmd_buf, "splash") == 0) ui_show_screen(SCREEN_SPLASH);
            else if (strcmp(cmd_buf, "mode") == 0) cycle_provider();
            else if (strcmp(cmd_buf, "pet") == 0) {
                splash_pet_next();
                ui_flash_hint(splash_pet_name(), 1600);
            }
            else if (strcmp(cmd_buf, "hold") == 0) {
                const bool paused = !splash_paused();
                splash_set_paused(paused);
                ui_flash_hint(paused ? "Pets held" : "Pets live", 1600);
                Serial.printf("pets: %s\n", paused ? "held" : "live");
            }
            else if (strcmp(cmd_buf, "auto") == 0) {
                theme_cycle_autoflip();
                autoflip_last_ms = millis();
                ui_flash_hint(theme_autoflip_label(), 1600);
                Serial.printf("autoflip: %s\n", theme_autoflip_label());
            }
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

// Each board provides this. Must bring up the shared I2C bus (Wire.begin
// with the board's SDA/SCL pins) and any board-private hardware that has
// to settle before display/touch (e.g. an IO expander gating the LCD
// reset line). Called exactly once at the start of setup().
extern "C" void board_init(void);

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("{\"ready\":true}");

    board_init();

    display_hal_init();
    display_hal_begin();
    idle_init();        // takes over panel brightness and starts the idle timer
    theme_init();       // restore the provider mode before anything reads the theme
    brightness_init();  // load the user's saved brightness level and apply via idle

    power_hal_init();
    imu_hal_init();
    sound_hal_init();
    touch_hal_init();

    // ---- LVGL ----
    const int W = board_caps().width;
    const int H = board_caps().height;

    lv_init();
    lv_tick_set_cb(my_tick);

    buf1 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);
    buf2 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);

    lv_display_t* disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, W * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

    ble_init();
    input_hal_init();

    ui_init();
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());
    ui_update_battery(power_hal_battery_pct(), power_hal_is_charging());
    ui_show_screen(SCREEN_SPLASH);

    Serial.printf("Dashboard ready (%s, %dx%d), waiting for data on BLE...\n",
        board_caps().name, W, H);
}

static ble_state_t last_ble_state = BLE_STATE_INIT;

// Hold-to-pair gesture: hold the PWR button ~3s, then RELEASE → clear all BLE
// bonds and re-advertise. Clearing on *release* (not while held) is deliberate:
// holding to power the device OFF (AXP hardware shutdown at 8s) must not wipe
// the bond — a power-off hold never releases before shutdown. To stop a
// "chicken-out" release just before 8s from pairing, the gesture disarms at 6s.
//
//   ~1.5s long-press edge → PENDING
//   3.0s (+1500)          → ARMED   (release from here clears bonds)
//   6.0s (+4500)          → DISARMED (no clear; AXP powers off at 8s)
#define PAIR_ARM_AFTER_LONG_MS    1500   // 3.0s total
#define PAIR_DISARM_AFTER_LONG_MS 4500   // 6.0s total
enum pair_state_t { PAIR_IDLE, PAIR_PENDING, PAIR_ARMED };
static pair_state_t pair_state        = PAIR_IDLE;
static uint32_t     pair_long_seen_ms = 0;

static void pair_tick(void) {
    if (pair_state == PAIR_IDLE && power_hal_pwr_long_pressed()) {
        pair_state = PAIR_PENDING;
        pair_long_seen_ms = millis();
        (void)power_hal_pwr_released();  // drain any stale release edge
        Serial.println("PWR long-press: hold to ~3s then release to pair");
        return;
    }
    if (pair_state == PAIR_IDLE) return;

    if (power_hal_pwr_released()) {
        if (pair_state == PAIR_ARMED) {
            Serial.println("Pair: released in window — clearing bonds, advertising");
            ble_clear_bonds();
        } else {
            Serial.println("Pair: released too early — cancelled");
        }
        pair_state = PAIR_IDLE;
        return;
    }

    uint32_t held = millis() - pair_long_seen_ms;
    if (pair_state == PAIR_PENDING && held >= PAIR_ARM_AFTER_LONG_MS) {
        pair_state = PAIR_ARMED;
        Serial.println("Pair: armed — release to pair");
    } else if (pair_state == PAIR_ARMED && held >= PAIR_DISARM_AFTER_LONG_MS) {
        pair_state = PAIR_IDLE;  // power-off territory; don't pair
        Serial.println("Pair: disarmed (holding toward power-off)");
    }
}

void loop() {
    idle_tick();
    lv_timer_handler();
    ui_tick_anim();
    ble_tick();
    power_hal_tick();
    imu_hal_tick();
    sound_hal_tick();
    splash_tick();
    splash_mascot_tick();
    // Rotation transition (blank + ramp) would fight the idle fade — skip
    // ticks while the panel is dark. A rotation that happens during sleep
    // is detected by the next tick after wake and ramped in then.
    if (!idle_is_asleep()) display_hal_tick();

    // ---- Physical buttons ----
    //   SECONDARY long-press → cycle provider mode (see MODE_HOLD_MS)
    //   PRIMARY   → HID Space  (Claude Code voice-mode PTT)
    //   SECONDARY → HID Shift+Tab  (mode toggle; only if the board has one)
    //   PWR       → on splash: cycle animations; on usage: cycle brightness;
    //               hold ~3s + release: pairing mode
    // First press from sleep is consumed as a wake-only event by
    // idle_consume_wake_press(); the normal action fires from the second
    // press. Activity bookkeeping happens inside idle_consume_wake_press
    // so no separate idle_note_activity() call is needed here.
    {
        // PRIMARY, like SECONDARY, now splits by hold length:
        //   tap  -> next pet (Codex only; the Claude side has one mascot)
        //   hold -> freeze/unfreeze every mascot, with a hint
        //
        // This retires the HID Space that was voice-mode push-to-talk. Both
        // HID bindings are now gone -- there is no third button on any
        // supported board, and the buttons are worth more driving the screen
        // in front of you than sending shortcuts to a terminal.
        {
            static bool     primary_was = false;
            static bool     primary_wake_swallowed = false;
            static uint32_t primary_down_ms = 0;
            static bool     primary_consumed = false;
            static bool     primary_raw_was = false;
            static uint32_t primary_edge_ms = 0;
            static bool     primary_now = false;
            const bool praw = input_hal_is_held(INPUT_BTN_PRIMARY);
            if (praw != primary_raw_was) {
                primary_raw_was = praw;
                primary_edge_ms = millis();
            } else if (praw != primary_now &&
                       millis() - primary_edge_ms >= SECONDARY_DEBOUNCE_MS) {
                primary_now = praw;
            }

            if (primary_now && !primary_was) {
                primary_down_ms = millis();
                primary_consumed = false;
                if (idle_consume_wake_press()) primary_wake_swallowed = true;
            } else if (primary_now && !primary_consumed &&
                       !primary_wake_swallowed &&
                       millis() - primary_down_ms >= MODE_HOLD_MS) {
                const bool paused = !splash_paused();
                splash_set_paused(paused);
                ui_flash_hint(paused ? "Pets held" : "Pets live", 1600);
                primary_consumed = true;
                Serial.printf("pets: %s\n", paused ? "held" : "live");
            } else if (!primary_now && primary_was) {
                if (primary_wake_swallowed)   primary_wake_swallowed = false;
                else if (!primary_consumed && theme_mode() == THEME_MODE_CODEX) {
                    splash_pet_next();
                    ui_flash_hint(splash_pet_name(), 1600);
                }
            }
            primary_was = primary_now;
        }

        if (board_caps().button_count >= 2) {
            // SECONDARY carries two actions, split by how long it is held:
            // a tap flips the provider now, a hold turns auto-flip on or off.
            // This retires the HID Shift+Tab that used to live on the tap --
            // there is no third button on any supported board to move it to.
            //
            // The hold fires on the threshold rather than on release, so the
            // gesture confirms itself under your thumb; the hint exists
            // because turning auto-flip ON has no other visible effect for
            // two minutes.
            static bool     secondary_was = false;
            static bool     secondary_wake_swallowed = false;
            static uint32_t secondary_down_ms = 0;
            static bool     secondary_consumed = false;
            static bool     secondary_raw_was = false;
            static uint32_t secondary_edge_ms = 0;
            static bool     secondary_now = false;
            const bool raw = input_hal_is_held(INPUT_BTN_SECONDARY);
            if (raw != secondary_raw_was) {
                secondary_raw_was = raw;
                secondary_edge_ms = millis();
            } else if (raw != secondary_now &&
                       millis() - secondary_edge_ms >= SECONDARY_DEBOUNCE_MS) {
                secondary_now = raw;
            }

            if (secondary_now && !secondary_was) {
                secondary_down_ms = millis();
                secondary_consumed = false;
                if (idle_consume_wake_press()) secondary_wake_swallowed = true;
            } else if (secondary_now && !secondary_wake_swallowed &&
                       millis() - secondary_down_ms >= MODE_HOLD_MS) {
                theme_cycle_autoflip();
                autoflip_last_ms = millis();
                ui_flash_hint(theme_autoflip_label(), 1600);
                secondary_consumed = true;
                Serial.printf("autoflip: %s\n", theme_autoflip_label());
                secondary_down_ms = millis();   // next step after another hold
            } else if (!secondary_now && secondary_was) {
                if (secondary_wake_swallowed)   secondary_wake_swallowed = false;
                else if (!secondary_consumed)   cycle_provider();
            }
            secondary_was = secondary_now;
        }

        // Auto-flip. Runs regardless of button count: a one-button board can
        // still be put into auto-flip over serial, and this is its only way to
        // reach the second provider at all.
        const uint32_t flip_ms = theme_autoflip_ms();
        if (flip_ms && millis() - autoflip_last_ms >= flip_ms) {
            cycle_provider();
        }

        if (power_hal_pwr_pressed()) {
            if (!idle_consume_wake_press()) {
                // On splash: cycle animations. On the usage view: cycle
                // screen brightness (single non-splash view, no more screens).
                if (ui_get_current_screen() == SCREEN_SPLASH) splash_next();
                else                                          brightness_cycle();
            }
        }

        pair_tick();
    }

    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    static int  last_pct      = -2;
    static bool last_charging = false;
    int  pct      = power_hal_battery_pct();
    bool charging = power_hal_is_charging();
    if (pct != last_pct || charging != last_charging) {
        if (pct != last_pct) ble_set_battery_level(pct);
        last_pct = pct;
        last_charging = charging;
        ui_update_battery(pct, charging);
    }

    check_serial_cmd();

    if (ble_has_data()) {
        if (parse_json(ble_get_data())) {
            int g_before = usage_rate_group();
            bool session_reset = usage_rate_sample(active_usage()->session_pct);
            int g_after = usage_rate_group();
            // 5-hour session limit refilled → chime so the user knows they can
            // use Claude again (no-op on boards without a buzzer). Gated on the
            // daemon's opt-in `chime` config; the `buzz` serial cmd ignores it.
            if (session_reset && active_usage()->chime) {
                Serial.println("session reset detected — chime");
                sound_hal_play_reset();
            }
            if (g_after != g_before) {
                Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
                    g_before, g_after, active_usage()->session_pct);
                if (splash_is_active()) splash_pick_for_current_rate();
            }
            ui_update(active_usage());
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }

    delay(5);
}
