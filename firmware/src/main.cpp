#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include "data.h"
#include "ui.h"
#include "ble.h"
#include "splash.h"
#include "mascot.h"
#include "clawd_director.h"
#include "usage_rate.h"
#include "signals.h"
#include "payload.h"
#include "minimal_mode.h"
#include "build_info.h"
#include "idle.h"
#include "idle_cfg.h"
#include "brightness.h"
#include "perf.h"
#include "pair_hold.h"

#include "hal/board_caps.h"
#include "hal/display_hal.h"
#include "hal/touch_hal.h"
#include "hal/input_hal.h"
#include "hal/power_hal.h"
#include "hal/imu_hal.h"
#include "hal/sound_hal.h"
#include "hal/board_info_hal.h"

static UsageData usage = {};

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
//           panel is dark, so pets/sleeves can't wake it overnight and the
//           director can't act on taps nobody can see.
// A finger is on the panel (after the wake filter below); read by the
// touch-hold re-pair on boards without a PWR button.
static bool touch_down = false;

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

    touch_down = pressed;
    if (pressed) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// Parse a JSON line into UsageData (payload.cpp holds the pure parser).
static bool parse_json(const char* json, UsageData* out) {
    const char* err = nullptr;
    if (!payload_parse(json, out, &err)) {
        Serial.printf("JSON parse error: %s\n", err ? err : "?");
        return false;
    }
    return true;
}

// Forget every bonded host and advertise again, so another computer (or
// this one, after removing the device) can pair. The PWR hold gesture, the
// touch hold on boards without PWR, and the `unpair` serial command all end
// here.
static void clear_pairing(const char* why) {
    Serial.printf("Pair: clearing bonds (%s), advertising\n", why);
    ble_clear_bonds();
    ui_set_pair_hint(PAIR_HINT_CLEARED);
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

// One JSON line identifying this build, for the desktop flasher's first
// detection probe. Keys are fixed: env, ver, rev, link.
static void send_info() {
    Serial.printf("{\"env\":\"%s\",\"ver\":\"%s\",\"rev\":\"%s\",\"link\":\"ble\"}\n",
                  CLAWD_ENV, CLAWD_VERSION, board_hal_rev());
}

static void check_serial_cmd() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            if (strcmp(cmd_buf, "screenshot") == 0) send_screenshot();
            else if (strcmp(cmd_buf, "chime") == 0 || strcmp(cmd_buf, "buzz") == 0) sound_hal_chime();
            else if (strncmp(cmd_buf, "chime ", 6) == 0) {
                // Dev tool for tuning a board's speaker: `chime <gain> [volume]`,
                // e.g. `chime 0.6 70` (until reboot).
                float g = 0;
                int v = -1;
                if (sscanf(cmd_buf + 6, "%f %d", &g, &v) >= 1) {
                    sound_hal_tune(g, v);
                    Serial.printf("{\"chime\":{\"gain\":%.2f,\"volume\":%d}}\n", g, v);
                } else {
                    Serial.println("usage: chime [<gain 0..1> [<volume 0..100>]]");
                }
            }
            else if (strcmp(cmd_buf, "info") == 0)  send_info();
            else if (strcmp(cmd_buf, "perf") == 0)  perf_print();
            else if (strcmp(cmd_buf, "ble") == 0) ble_print_status();
            else if (strcmp(cmd_buf, "disconnect") == 0) ble_disconnect_all();
            else if (strcmp(cmd_buf, "unpair") == 0) {
                clear_pairing("serial");
                Serial.println("{\"unpair\":true}");
            }
            else if (strncmp(cmd_buf, "clawd ", 6) == 0) {
                const bool ok = director_debug(cmd_buf + 6);
                Serial.printf("{\"clawd\":\"%s\",\"ok\":%s}\n", cmd_buf + 6, ok ? "true" : "false");
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
    perf_attach(disp, LV_DEF_REFR_PERIOD);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

    ble_init();
    input_hal_init();

    ui_init();
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());
    ui_update_battery(power_hal_battery_pct(), power_hal_is_charging(), power_hal_is_vbus_in());
    director_init();    // rooms + beats; boots on the meters (home)

    Serial.printf("Dashboard ready (%s, %dx%d) %lu ms after boot, waiting for data on BLE...\n",
        board_caps().name, W, H, (unsigned long)millis());
}

static ble_state_t last_ble_state = BLE_STATE_INIT;
static int shown_group = 0;   // mood group last logged

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
            clear_pairing("PWR hold");
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

// Boards without a PWR button: a 5 s touch hold on the pairing hint clears
// the bonds (pair_hold.h). Only counts while the hint is what you see.
static PairHold touch_hold = { PAIR_HOLD_UP, false, 0 };

static void touch_pair_tick(void) {
    if (board_caps().has_pwr_button) return;
    // Taps on the hint don't make Clawd peek (the face would cover it).
    director_set_pairing_touch(ui_pair_hint_visible());
    const bool enabled = ui_pair_hint_visible() && director_room() == ROOM_METERS && !idle_is_asleep();
    switch (pair_hold_step(&touch_hold, enabled, touch_down, millis())) {
    case PAIR_HOLD_HOLDING:
        Serial.println("Pair: touch hold, keep holding to clear the pairing");
        ui_set_pair_hint(PAIR_HINT_HOLDING);
        break;
    case PAIR_HOLD_CANCEL:
        Serial.println("Pair: touch hold released early, cancelled");
        ui_set_pair_hint(PAIR_HINT_NORMAL);
        break;
    case PAIR_HOLD_FIRE:
        clear_pairing("touch hold");
        break;
    case PAIR_HOLD_NONE:
        break;
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
    mascot_tick();
    director_tick();
    if (director_take_dismissed()) ble_send_dismiss();
    // Rotation transition (blank + ramp) would fight the idle fade — skip
    // ticks while the panel is dark. A rotation that happens during sleep
    // is detected by the next tick after wake and ramped in then.
    if (!idle_is_asleep()) display_hal_tick();

    // ---- Physical buttons ----
    //   PRIMARY   → HID Space  (Claude Code voice-mode PTT)
    //   SECONDARY → HID Shift+Tab  (mode toggle; only if the board has one)
    //   PWR       → on the stage: next scene in the group; elsewhere: cycle
    //               brightness; hold ~3s + release: pairing mode
    // Any press also tells the director you're here (wakes the screensaver).
    // First press from sleep is consumed as a wake-only event by
    // idle_consume_wake_press(); the normal action fires from the second
    // press. Activity bookkeeping happens inside idle_consume_wake_press
    // so no separate idle_note_activity() call is needed here.
    {
        static bool primary_was = false;
        static bool primary_wake_swallowed = false;
        bool primary_now = input_hal_is_held(INPUT_BTN_PRIMARY);
        if (primary_now != primary_was) {
            if (primary_now) {
                if (idle_consume_wake_press()) primary_wake_swallowed = true;
                else { ble_keyboard_press(0x2C, 0); director_button(); }  // HID Space, no mods
            } else {
                if (primary_wake_swallowed) primary_wake_swallowed = false;
                else                        ble_keyboard_release();
            }
            primary_was = primary_now;
        }

        if (board_caps().button_count >= 2) {
            static bool secondary_was = false;
            static bool secondary_wake_swallowed = false;
            bool secondary_now = input_hal_is_held(INPUT_BTN_SECONDARY);
            if (secondary_now != secondary_was) {
                if (secondary_now) {
                    if (idle_consume_wake_press()) secondary_wake_swallowed = true;
                    else { ble_keyboard_press(0x2B, 0x02); director_button(); }  // HID Tab + LEFT_SHIFT
                } else {
                    if (secondary_wake_swallowed) secondary_wake_swallowed = false;
                    else                          ble_keyboard_release();
                }
                secondary_was = secondary_now;
            }
        }

        if (power_hal_pwr_pressed()) {
            if (!idle_consume_wake_press()) {
                // On the stage PWR swaps the scene and stays in the room (a
                // screensaver calm dip steps to the other calm scene);
                // anywhere else it cycles brightness and counts as you being here.
                if (director_room() == ROOM_STAGE) director_stage_next();
                else { brightness_cycle(); director_button(); }
            }
        }

        pair_tick();
        touch_pair_tick();
    }

    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    static int  last_pct      = -2;
    static bool last_charging = false;
    static bool last_vbus     = false;
    int  pct      = power_hal_battery_pct();
    bool charging = power_hal_is_charging();
    bool vbus     = power_hal_is_vbus_in();
    if (pct != last_pct || charging != last_charging || vbus != last_vbus) {
        if (pct != last_pct) ble_set_battery_level(pct);
        last_pct = pct;
        last_charging = charging;
        last_vbus = vbus;
        ui_update_battery(pct, charging, vbus);
    }

    check_serial_cmd();

    if (ble_has_data()) {
        if (parse_json(ble_get_data(), &usage)) {
            // Fast + slow clock bookkeeping: rate sample (replays skipped),
            // hook activity lift, event dedupe. See signals.h.
            const SignalsUpdate sig = signals_ingest(&usage);
            const bool session_reset = sig.session_reset;
            const int g_before = sig.group_before;
            const int g_after  = sig.group_after;
            shown_group = g_after;
            if (sig.event_fired)
                Serial.printf("[%lu] hook event '%c' (#%lu)\n", (unsigned long)millis(),
                              sig.event ? sig.event : '?', (unsigned long)sig.event_seq);
            // 5-hour session limit refilled → ding so the user knows they can
            // use Claude again (no-op on boards without sound). Gated on the
            // app's opt-in `c` (reset_chime); the `chime` serial cmd ignores it.
            if (session_reset && usage.chime) {
                Serial.println("session reset detected — chime");
                sound_hal_chime();
            }
            // Claude Code hook fired on the host: the same ding. Not gated on
            // `c`: the app only sets `b` when its alert chime is on.
            if (usage.beep) {
                Serial.println("attention — chime");
                sound_hal_chime();
            }
            ui_update(&usage);
            // A prompt (or you're back after a gap): you're at the computer,
            // so a dimmed or dark panel wakes before Clawd notices you. A
            // Stop or tool activity doesn't: Claude working alone isn't you.
            if (sig.event_fired && (sig.event == 'p' || sig.event == 'b'))
                idle_note_activity();
            director_on_payload(&usage, &sig);
            if (usage.alert_msg[0]) {
                Serial.println("attention — mascot");
                idle_note_activity();
                director_alert(usage.alert_msg);
            }
            if (usage.alert_clear) director_alert_clear();
            if (g_after != g_before)
                Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
                    g_before, g_after, usage.session_pct);
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }

    // The mood group also moves without a payload: the 15-min window decays
    // to Idle on its own and a hook activity lift expires. Nothing needs to
    // react (the corner and the stage read the group when they pick), but log
    // it so the serial trace matches what Clawd does.
    {
        static uint32_t last_check = 0;
        if (millis() - last_check >= 1000) {
            last_check = millis();
            const int g = usage_rate_group();
            if (g != shown_group) {
                Serial.printf("usage rate: group %d -> %d (decay)\n", shown_group, g);
                shown_group = g;
            }
        }
    }

    delay(5);
}
