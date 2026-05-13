#include <Arduino.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include "display_cfg.h"
#include "data.h"
#include "ui.h"
#include "ble.h"
#include "power.h"
#include "imu.h"
#include "splash.h"
#include "usage_rate.h"
#include "audio.h"

// Physical buttons (global, screen-independent):
//   BTN_BACK   (GPIO 0)  — left,  send Space (Claude Code voice mode push-to-talk)
//   BTN_FWD    (GPIO 18) — right, send Shift+Tab (Claude Code mode toggle)
//   AXP PWR    (PMU)     — middle, cycle screens; on splash, cycle animations
#define BTN_BACK 0
#define BTN_FWD  18

// ---- Hardware objects ----
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_SH8601 *gfx = new Arduino_SH8601(
    bus, GFX_NOT_DEFINED /* RST via XCA9554 */, 0 /* rotation */,
    LCD_WIDTH, LCD_HEIGHT);
XPowersPMU pmu;
SensorQMI8658 imu;
Adafruit_XCA9554 io_expander;

// FT3168 touch — Arduino_DriveBus uses a shared I2C bus object
static std::shared_ptr<Arduino_IIC_DriveBus> _ft_bus;
static void IRAM_ATTR touch_isr(void);
std::unique_ptr<Arduino_IIC> touch_ft;

static UsageData usage = {};

// ---- Display sleep state ----
// AMOLED draws power per lit pixel, so brightness=0 is effectively off.
// On wake we restore the last brightness (default 200).
static bool     display_asleep = false;
static uint8_t  display_brightness = 200;

static bool display_is_asleep(void) { return display_asleep; }

static void display_set_asleep(bool sleep) {
    if (sleep == display_asleep) return;
    display_asleep = sleep;
    if (sleep) {
        gfx->setBrightness(0);
        Serial.println("display: sleeping");
    } else {
        gfx->setBrightness(display_brightness);
        Serial.println("display: awake");
    }
}

// ---- Touch shared state ----
static volatile bool     touch_pressed = false;
static volatile uint16_t touch_x = 0;
static volatile uint16_t touch_y = 0;

static void IRAM_ATTR touch_isr(void) {
    if (touch_ft) touch_ft->IIC_Interrupt_Flag = true;
}

static void touch_read() {
    if (!touch_ft || !touch_ft->IIC_Interrupt_Flag) return;
    touch_ft->IIC_Interrupt_Flag = false;

    int32_t tx = touch_ft->IIC_Read_Device_Value(
        Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    int32_t ty = touch_ft->IIC_Read_Device_Value(
        Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
    int32_t n  = touch_ft->IIC_Read_Device_Value(
        Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);

    if (n > 0 && tx >= 0 && ty >= 0) {
        touch_pressed = true;
        touch_x = (uint16_t)tx;
        touch_y = (uint16_t)ty;
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

// Parse a JSON line into UsageData
static bool parse_json(const char* json, UsageData* out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }

    out->session_pct = doc["s"] | 0.0f;
    out->session_reset_mins = doc["sr"] | -1;
    out->weekly_pct = doc["w"] | 0.0f;
    out->weekly_reset_mins = doc["wr"] | -1;
    strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
    out->ok = doc["ok"] | false;
    strlcpy(out->attn_msg, doc["at"] | "", sizeof(out->attn_msg));

    // Sessions array: { p: project, s: state-letter, m?: message }
    out->sessions_count = 0;
    JsonArrayConst arr = doc["sess"].as<JsonArrayConst>();
    for (JsonObjectConst s : arr) {
        if (out->sessions_count >= MAX_SESSIONS) break;
        SessionInfo* si = &out->sessions[out->sessions_count++];
        strlcpy(si->proj, s["p"] | "session", sizeof(si->proj));
        const char* sc = s["s"] | "i";
        si->state = (sc[0] == 'w') ? SESS_WAITING
                  : (sc[0] == 'k') ? SESS_WORKING
                  : SESS_IDLE;
        strlcpy(si->msg, s["m"] | "", sizeof(si->msg));
    }

    out->valid = true;
    return true;
}

// Serial command buffer
#define CMD_BUF_SIZE 64
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

static void send_screenshot() {
    const uint32_t w = LCD_WIDTH, h = LCD_HEIGHT;
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

    Serial.printf("SCREENSHOT_START %lu %lu %lu\n", (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");

    heap_caps_free(sbuf);
}

static void check_serial_cmd() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            if (strcmp(cmd_buf, "screenshot") == 0) {
                send_screenshot();
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

    // Init I2C (shared by touch + PMU + IMU + I/O expander)
    Wire.begin(IIC_SDA, IIC_SCL);

    // Init I/O expander — pulses LCD_RST / TP_RST / one more reset line.
    // The hardware reset must happen before the display and touch begin().
    if (!io_expander.begin(XCA9554_ADDR)) {
        Serial.println("XCA9554 init failed");
    } else {
        io_expander.pinMode(0, OUTPUT);
        io_expander.pinMode(1, OUTPUT);
        io_expander.pinMode(2, OUTPUT);
        io_expander.digitalWrite(0, LOW);
        io_expander.digitalWrite(1, LOW);
        io_expander.digitalWrite(2, LOW);
        delay(20);
        io_expander.digitalWrite(0, HIGH);
        io_expander.digitalWrite(1, HIGH);
        io_expander.digitalWrite(2, HIGH);
        delay(20);
        Serial.println("XCA9554 init OK");
    }

    // Init display
    gfx->begin();
    gfx->fillScreen(0x0000);
    gfx->setBrightness(200);

    // Init PMU
    power_init();

    // Audio (ES8311 + I2S). Beep on Notification attn rising edge.
    audio_init();

    // Init IMU (accelerometer; rotation is locked to 0 on this non-square panel)
    imu_init();

    // Init touch (FT3168 via Arduino_DriveBus)
    _ft_bus = std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);
    touch_ft.reset(new Arduino_FT3x68(
        _ft_bus, FT3168_DEVICE_ADDRESS,
        DRIVEBUS_DEFAULT_VALUE /* RST: via expander */, TP_INT, touch_isr));
    if (!touch_ft->begin()) {
        Serial.println("FT3168 init failed");
    } else {
        Serial.println("FT3168 init OK");
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

    // Init BLE data channel
    ble_init();

    // Physical buttons: back (GPIO 0) and forward (GPIO 18)
    pinMode(BTN_BACK, INPUT_PULLUP);
    pinMode(BTN_FWD,  INPUT_PULLUP);

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
    ble_tick();
    power_tick();
    imu_tick();
    splash_tick();

    // Inputs:
    //   BOOT (GPIO 0)         → toggle display sleep (off/on)
    //   PWR  (AXP2101 PKEY)   → short: cycle screens / splash animations
    //                         → long:  shut the device down via PMU
    //
    // While the display is asleep, ANY touch wakes it; same for an incoming
    // attention event (handled where we parse BLE payloads).
    {
        static bool back_was = false;
        bool back_now = (digitalRead(BTN_BACK) == LOW);

        if (back_now && !back_was) {
            display_set_asleep(!display_is_asleep());
        }
        back_was = back_now;

        if (display_is_asleep() && touch_pressed) {
            display_set_asleep(false);
        }

        if (power_pwr_long_pressed()) {
            Serial.println("PWR long press → shutdown");
            gfx->setBrightness(0);
            delay(80);
            power_shutdown();
            // pmu.shutdown() cuts power; control never returns. If for any
            // reason it does (e.g. AXP isn't actually wired to enable rails
            // for this dev variant), fall through to a reboot.
            esp_restart();
        }

        if (power_pwr_pressed()) {
            if (display_is_asleep()) display_set_asleep(false);
            else if (ui_get_current_screen() == SCREEN_SPLASH) splash_next();
            else                                               ui_cycle_screen();
        }
    }

    handle_rotation_change();

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

    // Check for serial commands (screenshot, etc.)
    check_serial_cmd();

    // Process incoming BLE data
    if (ble_has_data()) {
        if (parse_json(ble_get_data(), &usage)) {
            int g_before = usage_rate_group();
            usage_rate_sample(usage.session_pct);
            int g_after = usage_rate_group();
            if (g_after != g_before) {
                Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
                    g_before, g_after, usage.session_pct);
                if (splash_is_active()) splash_pick_for_current_rate();
            }
            ui_update(&usage);
            // Detect attention rising edge to beep exactly once per
            // "Claude needs you" event, not on every payload refresh.
            // Also wakes the display if it was asleep.
            static char last_attn[96] = {0};
            bool prev_attn = last_attn[0] != '\0';
            bool now_attn  = usage.attn_msg[0] != '\0';
            if (!prev_attn && now_attn) {
                audio_attn_chime();
                display_set_asleep(false);
            }
            strlcpy(last_attn, usage.attn_msg, sizeof(last_attn));
            ui_set_attn(usage.attn_msg);
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }

    delay(5);
}
