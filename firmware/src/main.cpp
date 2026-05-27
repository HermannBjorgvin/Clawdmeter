#include <Arduino.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include "display_cfg.h"
#include "data.h"
#include "ui.h"
// ---- Transport selector ----
// Default: USB CDC (PR #26's `serial_link`). Set -DCLAWDMETER_TRANSPORT_WIFI
// in platformio.ini build_flags to use the WiFi/HTTP bridge instead. Both
// transports expose the same API — only the function names differ.
#ifdef CLAWDMETER_TRANSPORT_WIFI
  #include "wifi_link.h"
  #define link_init            wifi_link_init
  #define link_tick            wifi_link_tick
  #define link_get_state       wifi_link_get_state
  #define link_get_port_name   wifi_link_get_port_name
  #define link_has_data        wifi_link_has_data
  #define link_get_data        wifi_link_get_data
  #define link_send_ack        wifi_link_send_ack
  #define link_send_nack       wifi_link_send_nack
#else
  #include "serial_link.h"
  #define link_init            serial_link_init
  #define link_tick            serial_link_tick
  #define link_get_state       serial_link_get_state
  #define link_get_port_name   serial_link_get_port_name
  #define link_has_data        serial_link_has_data
  #define link_get_data        serial_link_get_data
  #define link_send_ack        serial_link_send_ack
  #define link_send_nack       serial_link_send_nack
#endif
#include "power.h"
#include "imu.h"
#include "splash.h"
#include "usage_rate.h"

// Physical buttons (global, screen-independent):
//   BTN_BACK   (GPIO 0)  — left,  reserved (was BLE HID Space, no-op on USB build)
//   BTN_FWD    (GPIO 18) — right, reserved (was BLE HID Shift+Tab, no-op on USB build)
//   AXP PWR    (PMU)     — middle, cycle screens; on splash, cycle animations
// TODO(usb-transport): wire left/right buttons to USB HID composite for
// equivalent Space / Shift+Tab keystrokes.
#define BTN_BACK 0
#define BTN_FWD  18

// ---- Hardware objects ----
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, LCD_RESET, 0 /* rotation */,
    LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 0);
TouchDrvCST92xx touch;
XPowersPMU pmu;
SensorQMI8658 imu;

static UsageData usage = {};

// ---- Touch interrupt + shared state ----
static volatile bool     touch_pressed = false;
static volatile uint16_t touch_x = 0;
static volatile uint16_t touch_y = 0;
static volatile bool     touch_data_ready = false;

static void IRAM_ATTR touch_isr(void) {
    touch_data_ready = true;
}

static void touch_read() {
    if (!touch_data_ready) return;
    touch_data_ready = false;

    int16_t tx[5], ty[5];
    uint8_t n = touch.getPoint(tx, ty, touch.getSupportTouchPoint());
    if (n > 0) {
        touch_pressed = true;
        touch_x = (uint16_t)tx[0];
        touch_y = (uint16_t)ty[0];
    } else {
        touch_pressed = false;
    }
}

// ---- LVGL draw buffers (PSRAM-backed, partial render) ----
#define BUF_LINES 40
static uint16_t *buf1 = nullptr;
static uint16_t *buf2 = nullptr;
// rot_buf for strip rotation — max size is 480×480 (full invalidation case)
// but typical partial strips are much smaller
static uint16_t *rot_buf = nullptr;

// LVGL tick callback
static uint32_t my_tick(void) {
    return millis();
}

// Rotate a w×h strip and compute destination coordinates on the 480×480 display.
// src pixels are in row-major order for the rectangle (sx, sy, w, h).
// Output goes to rot_buf in row-major order for the destination rectangle.
static void rotate_strip(const uint16_t *src, int32_t w, int32_t h,
                         int32_t sx, int32_t sy, uint8_t r,
                         int32_t *dx, int32_t *dy, int32_t *dw, int32_t *dh) {
    const int S = LCD_WIDTH;  // 480

    switch (r) {
    case 1: { // 90° CW: (x,y) -> (S-1-y, x)
        *dw = h; *dh = w;
        *dx = S - sy - h;
        *dy = sx;
        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x++) {
                // src(x,y) -> dst(h-1-y, x)
                rot_buf[x * h + (h - 1 - y)] = src[y * w + x];
            }
        }
        break;
    }
    case 2: { // 180°: (x,y) -> (S-1-x, S-1-y)
        *dw = w; *dh = h;
        *dx = S - sx - w;
        *dy = S - sy - h;
        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x++) {
                rot_buf[(h - 1 - y) * w + (w - 1 - x)] = src[y * w + x];
            }
        }
        break;
    }
    case 3: { // 270° CW: (x,y) -> (y, S-1-x)
        *dw = h; *dh = w;
        *dx = sy;
        *dy = S - sx - w;
        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x++) {
                // src(x,y) -> dst(y, w-1-x)
                rot_buf[(w - 1 - x) * h + y] = src[y * w + x];
            }
        }
        break;
    }
    default:
        *dx = sx; *dy = sy; *dw = w; *dh = h;
        break;
    }
}

// LVGL flush callback — rotates partial strips and writes to display
static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    uint16_t *src = (uint16_t*)px_map;
    uint8_t r = imu_get_rotation();

    if (r == 0) {
        gfx->draw16bitRGBBitmap(area->x1, area->y1, src, w, h);
    } else {
        int32_t dx, dy, dw, dh;
        rotate_strip(src, w, h, area->x1, area->y1, r, &dx, &dy, &dw, &dh);
        gfx->draw16bitRGBBitmap(dx, dy, rot_buf, dw, dh);
    }
    lv_display_flush_ready(disp);
}

// CO5300 requires even-aligned flush regions
static void rounder_cb(lv_event_t* e) {
    lv_area_t *area = (lv_area_t*)lv_event_get_param(e);
    area->x1 = area->x1 & ~1;
    area->y1 = area->y1 & ~1;
    area->x2 = area->x2 | 1;
    area->y2 = area->y2 | 1;
}

// LVGL touch callback
static void my_touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    if (touch_pressed) {
        data->point.x = touch_x;
        data->point.y = touch_y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// Status string lookup for todo_status_t — daemon sends integer codes
// (0=pending, 1=in_progress, 2=completed) per the PR #22 wire schema.
static todo_status_t todo_status_from_int(int s) {
    if (s == 1) return TODO_IN_PROGRESS;
    if (s == 2) return TODO_COMPLETED;
    return TODO_PENDING;
}

// Parse a JSON line into UsageData (always) and optionally ActivityData
// from the "sessions" array. The act_out parameter may be null when the
// caller doesn't care about activity.
//
// Two-pass design for robustness:
//   1. Filtered parse that extracts only the Usage scalars. Even if the
//      full payload is malformed/oversized/unicode-laden, the small
//      filtered doc parses reliably.
//   2. Best-effort full parse for the sessions array. A failure here leaves
//      ActivityData empty but Usage is still valid and we still ACK the
//      payload upstream.
static bool parse_json(const char* json, UsageData* out, ActivityData* act_out = nullptr) {
    // ---- Pass 1: filtered Usage-only parse (tiny doc, always succeeds) ----
    JsonDocument filter;
    filter["s"] = true;
    filter["sr"] = true;
    filter["w"] = true;
    filter["wr"] = true;
    filter["st"] = true;
    filter["ok"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json,
        DeserializationOption::Filter(filter));
    if (err) {
        Serial.printf("JSON parse error (usage): %s (len=%d)\n", err.c_str(), (int)strlen(json));
        return false;
    }

    out->session_pct = doc["s"] | 0.0f;
    out->session_reset_mins = doc["sr"] | -1;
    out->weekly_pct = doc["w"] | 0.0f;
    out->weekly_reset_mins = doc["wr"] | -1;
    strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
    out->ok = doc["ok"] | false;
    out->valid = true;

    // ---- Pass 2: best-effort sessions parse ----
    if (act_out) {
        memset(act_out, 0, sizeof(*act_out));
        act_out->valid = true;

        JsonDocument sess_doc;
        DeserializationError sess_err = deserializeJson(sess_doc, json);
        if (sess_err) {
            Serial.printf("Sessions parse skipped: %s\n", sess_err.c_str());
            return true;  // Usage still valid -> caller will ACK
        }

        JsonArrayConst sessions = sess_doc["sessions"];
        if (!sessions.isNull()) {
            uint8_t s_idx = 0;
            for (JsonObjectConst s : sessions) {
                if (s_idx >= MAX_SESSIONS) break;
                SessionData& sd = act_out->sessions[s_idx];
                strlcpy(sd.project,           s["p"]  | "", sizeof(sd.project));
                strlcpy(sd.model,             s["m"]  | "", sizeof(sd.model));
                strlcpy(sd.last_prompt,       s["u"]  | "", sizeof(sd.last_prompt));
                strlcpy(sd.current_tool,      s["t"]  | "", sizeof(sd.current_tool));
                strlcpy(sd.current_tool_args, s["ta"] | "", sizeof(sd.current_tool_args));
                sd.phase            = (s["ph"] | 0) == 1 ? PHASE_RUNNING : PHASE_IDLE;
                sd.last_active_secs = s["la"] | 0;

                JsonArrayConst todos = s["td"];
                uint8_t t_idx = 0;
                if (!todos.isNull()) {
                    for (JsonObjectConst t : todos) {
                        if (t_idx >= MAX_TODOS_PER_SESSION) break;
                        TodoItem& ti = sd.todos[t_idx];
                        strlcpy(ti.content,     t["c"] | "", sizeof(ti.content));
                        strlcpy(ti.active_form, t["a"] | "", sizeof(ti.active_form));
                        ti.status = todo_status_from_int(t["s"] | 0);
                        t_idx++;
                    }
                }
                sd.todo_count = t_idx;
                s_idx++;
            }
            act_out->session_count = s_idx;
        }
    }
    return true;
}

void setup() {
    // Bump RX buffer before begin() so HWCDC honors it. Default ~256 bytes
    // truncates multi-session Activity payloads (~700-1000 bytes) between
    // link_tick() calls.
    Serial.setRxBufferSize(4096);
    Serial.begin(115200);
    delay(300);
    Serial.println("{\"ready\":true}");

    // Init I2C (shared by touch + PMU)
    Wire.begin(IIC_SDA, IIC_SCL);

    // Init display
    gfx->begin();
    gfx->fillScreen(0x0000);
    gfx->setBrightness(200);

    // Init PMU
    power_init();

    // Init IMU (accelerometer for auto-rotation)
    imu_init();

    // Init touch
    touch.setPins(TP_RST, TP_INT);
    if (!touch.begin(Wire, CST9220_ADDR, IIC_SDA, IIC_SCL)) {
        Serial.println("Touch init failed");
    } else {
        touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
        touch.setSwapXY(true);
        touch.setMirrorXY(true, false);
        attachInterrupt(TP_INT, touch_isr, FALLING);
        Serial.println("Touch init OK");
    }

    // Init LVGL
    lv_init();
    lv_tick_set_cb(my_tick);

    // Allocate PSRAM-backed partial render buffers
    buf1 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);
    buf2 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);
    // rot_buf needs to hold the largest possible strip after rotation
    // A 480×40 strip rotated 90° becomes 40×480, same pixel count
    rot_buf = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);

    lv_display_t* disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, LCD_WIDTH * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    // CO5300 even-alignment rounder
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

    // Init USB CDC data channel
    link_init();

    // Physical buttons: back (GPIO 0) and forward (GPIO 18)
    pinMode(BTN_BACK, INPUT_PULLUP);
    pinMode(BTN_FWD,  INPUT_PULLUP);

    // Build dashboard
    ui_init();

    // Show initial link status on Link screen
    ui_update_link_status(link_get_state(), link_get_port_name());

    // Show initial battery status
    ui_update_battery(power_battery_pct(), power_is_charging());

    ui_show_screen(SCREEN_SPLASH);

    Serial.println("Dashboard ready, waiting for data on USB CDC...");
}

static link_state_t last_link_state = LINK_STATE_INIT;

// Brightness ramp state for rotation transition
// On rotation change we blank the panel, force a full LVGL redraw at the
// new orientation, then ramp brightness back up over ~125ms so the
// transition reads as deliberate instead of as a glitch.
static void handle_rotation_change(void) {
    static uint8_t last_rotation = 0;
    static uint8_t  ramp_step = 0;  // 0=idle, 1-4=ramping
    static uint32_t ramp_last = 0;

    uint8_t rot = imu_get_rotation();
    if (rot != last_rotation) {
        gfx->setBrightness(0);
        last_rotation = rot;
        lv_obj_invalidate(lv_screen_active());
        ramp_step = 1;
        return;
    }

    if (ramp_step == 0) return;
    uint32_t now = millis();
    if (now - ramp_last < 25) return;
    ramp_last = now;

    static const uint8_t levels[] = {60, 120, 170, 200};
    gfx->setBrightness(levels[ramp_step - 1]);
    if (ramp_step >= 4) ramp_step = 0;
    else                ramp_step++;
}

void loop() {
    touch_read();
    lv_timer_handler();
    ui_tick_anim();
    link_tick();
    power_tick();
    imu_tick();
    splash_tick();

    // Three-button input (global, screen-independent):
    //   LEFT  (GPIO 0)  → Space (voice-mode push-to-talk; press & release tracked)
    //   RIGHT (GPIO 18) → Shift+Tab (Claude Code mode toggle)
    //   PWR   (AXP)     → cycle screens; on splash, cycle animations
    {
        static bool back_was = false, fwd_was = false;
        bool back_now = (digitalRead(BTN_BACK) == LOW);
        bool fwd_now  = (digitalRead(BTN_FWD)  == LOW);

        if (back_now != back_was) {
            // TODO(usb-transport): USB HID Space here.
            back_was = back_now;
        }
        if (fwd_now != fwd_was) {
            // TODO(usb-transport): USB HID Shift+Tab here.
            fwd_was = fwd_now;
        }

        if (power_pwr_pressed()) {
            if (ui_get_current_screen() == SCREEN_SPLASH) splash_next();
            else                                          ui_cycle_screen();
        }
    }

    handle_rotation_change();

    // Update link status on screen when state changes
    link_state_t ls = link_get_state();
    if (ls != last_link_state) {
        last_link_state = ls;
        ui_update_link_status(ls, link_get_port_name());
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

    // Process incoming serial data (screenshot cmd handled inside the tick)
    if (link_has_data()) {
        static ActivityData activity = {};
        if (parse_json(link_get_data(), &usage, &activity)) {
            int g_before = usage_rate_group();
            usage_rate_sample(usage.session_pct);
            int g_after = usage_rate_group();
            if (g_after != g_before) {
                Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
                    g_before, g_after, usage.session_pct);
                if (splash_is_active()) splash_pick_for_current_rate();
            }
            ui_update(&usage);
            ui_update_activity(&activity);
            link_send_ack();
        } else {
            link_send_nack();
        }
    }

    delay(5);
}
