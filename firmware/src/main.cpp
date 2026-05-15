#include <Arduino.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include "display_cfg.h"
#include "data.h"
#include "ui.h"
#include "ble.h"
// #include "power.h"  // TODO: Enable once AXP2101 presence confirmed
#include "imu.h"
#include "splash.h"
#include "usage_rate.h"

// Physical buttons (global, screen-independent):
//   BTN_BACK   (GPIO 0)  — left,  send Space (Claude Code voice mode push-to-talk)
//   BTN_FWD    (GPIO 18) — right, send Shift+Tab (Claude Code mode toggle)
//   AXP PWR    (PMU)     — middle, cycle screens; on splash, cycle animations
#define BTN_BACK 0
#define BTN_FWD  18

// ---- Hardware objects ----
// SH8601 uses QSPI. Reset pin is on the TCA9554 expander, not a direct GPIO —
// pass GFX_NOT_DEFINED so Arduino_GFX doesn't try to toggle a non-existent pin,
// and handle reset ourselves in io_expander_init().
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
// SH8601 sees the PHYSICAL panel (368×448 portrait). The landscape rotation
// happens in software in my_flush_cb, not in the controller.
Arduino_SH8601 *gfx = new Arduino_SH8601(
    bus, GFX_NOT_DEFINED /* reset on expander */, 0 /* rotation */,
    PANEL_W, PANEL_H);
TouchDrvFT6X36 touch;
SensorQMI8658 imu;

// Minimal TCA9554 driver — we only need to set all pins as outputs and drive
// LCD_RESET, DSI_PWR_EN, and TP_RESET. No need for the full bus master.
static bool tca9554_write_reg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

// Bring up the I/O expander, power on the AMOLED panel, and run a clean
// reset pulse for both LCD and touch.
static void io_expander_init(void) {
    // Configuration register (0x03): 0 = output, 1 = input. All outputs.
    if (!tca9554_write_reg(0x03, 0x00)) {
        Serial.println("TCA9554: not responding at 0x20");
        return;
    }
    // Output register (0x01): start with everything low (reset asserted).
    tca9554_write_reg(0x01, 0x00);
    delay(20);
    // Power on the panel (DSI_PWR_EN high), keep resets asserted.
    tca9554_write_reg(0x01, (1 << EXIO_LCD_PWR_EN));
    delay(50);
    // Release LCD_RESET and TP_RESET; panel power stays on.
    uint8_t out = (1 << EXIO_LCD_PWR_EN) |
                  (1 << EXIO_LCD_RESET)  |
                  (1 << EXIO_TP_RESET);
    tca9554_write_reg(0x01, out);
    delay(150);
    Serial.println("TCA9554 init OK (panel powered, resets released)");
}

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
        // Touch returns physical panel coords (0..PANEL_W × 0..PANEL_H).
        // Map them into logical landscape coords using the inverse of the
        // rotation applied in rotate_strip.
        uint16_t px = (uint16_t)tx[0];
        uint16_t py = (uint16_t)ty[0];
        uint8_t r = imu_get_rotation();
        if (r == 3) {
            // Inverse of 270° CW: physical (px, py) -> logical (PANEL_H - 1 - py, px)
            touch_x = (PANEL_H - 1) - py;
            touch_y = px;
        } else {
            // Inverse of 90° CW: physical (px, py) -> logical (py, PANEL_W - 1 - px)
            touch_x = py;
            touch_y = (PANEL_W - 1) - px;
        }
        touch_pressed = true;
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
// Landscape rotation: maps a strip from logical 448×368 landscape coords
// onto the physical 368×448 portrait panel. r=1 is 90° CW, r=3 is 270° CW.
// Any other value falls back to r=1 (default landscape).
static void rotate_strip(const uint16_t *src, int32_t w, int32_t h,
                         int32_t sx, int32_t sy, uint8_t r,
                         int32_t *dx, int32_t *dy, int32_t *dw, int32_t *dh) {
    if (r == 3) {
        // 270° CW: logical (x, y) -> physical (y, PANEL_H - 1 - x)
        *dw = h; *dh = w;
        *dx = sy;
        *dy = PANEL_H - sx - w;
        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x++) {
                rot_buf[(w - 1 - x) * h + y] = src[y * w + x];
            }
        }
    } else {
        // 90° CW (default): logical (x, y) -> physical (PANEL_W - 1 - y, x)
        *dw = h; *dh = w;
        *dx = PANEL_W - sy - h;
        *dy = sx;
        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x++) {
                rot_buf[x * h + (h - 1 - y)] = src[y * w + x];
            }
        }
    }
}

// LVGL flush callback — always rotates from logical landscape into the
// physical portrait panel. IMU picks 90° vs 270° based on which way the
// device is held.
static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    uint16_t *src = (uint16_t*)px_map;
    uint8_t r = imu_get_rotation();

    int32_t dx, dy, dw, dh;
    rotate_strip(src, w, h, area->x1, area->y1, r, &dx, &dy, &dw, &dh);
    gfx->draw16bitRGBBitmap(dx, dy, rot_buf, dw, dh);
    lv_display_flush_ready(disp);
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

    // Init I2C (shared by touch + I/O expander + future PMU/IMU/RTC)
    Wire.begin(IIC_SDA, IIC_SCL);

    // Bring up the I/O expander first — the AMOLED panel power rail is gated
    // by EXIO1 (DSI_PWR_EN), and the LCD reset line is on EXIO0. Without
    // this, gfx->begin() talks to an unpowered panel and the screen stays black.
    io_expander_init();

    // Init display
    gfx->begin();
    gfx->fillScreen(0x0000);
    gfx->setBrightness(200);

    // TODO: Enable once AXP2101 wiring confirmed
    // power_init();
    imu_init();

    // Init touch (FT3168)
    // Note: FT6X36 driver is used for FT3168 compatibility
    if (!touch.begin(Wire, FT3168_ADDR)) {
        Serial.println("Touch init failed");
    } else {
        // Touch returns physical panel coordinates; rotation is handled in
        // touch_read() based on imu_get_rotation().
        touch.setMaxCoordinates(PANEL_W, PANEL_H);
        touch.setSwapXY(false);
        touch.setMirrorXY(false, false);
        if (TP_INT != -1) {
            attachInterrupt(TP_INT, touch_isr, FALLING);
        }
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

    // TODO: Show battery status once power module confirmed
    // ui_update_battery(power_battery_pct(), power_is_charging());

    ui_show_screen(SCREEN_SPLASH);

    Serial.println("Dashboard ready, waiting for data on BLE...");
}

static ble_state_t last_ble_state = BLE_STATE_INIT;

// Brightness ramp state for rotation transition. On rotation change we blank
// the panel, force a full LVGL redraw at the new orientation, then ramp
// brightness back up over ~125ms so the transition reads as deliberate.
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
    // power_tick();  // TODO: Enable once AXP2101 confirmed
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
            if (back_now) ble_keyboard_press(0x2C, 0);  // HID Space, no mods
            else          ble_keyboard_release();
            back_was = back_now;
        }
        if (fwd_now != fwd_was) {
            if (fwd_now) ble_keyboard_press(0x2B, 0x02);  // HID Tab + LEFT_SHIFT
            else         ble_keyboard_release();
            fwd_was = fwd_now;
        }

        // TODO: Enable once AXP2101 confirmed
        // if (power_pwr_pressed()) {
        //     if (ui_get_current_screen() == SCREEN_SPLASH) splash_next();
        //     else                                          ui_cycle_screen();
        // }
    }

    handle_rotation_change();

    // Update BLE status on screen when state changes
    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    // TODO: Update battery indicator once power module confirmed
    // static int last_pct = -2;
    // static bool last_charging = false;
    // int pct = power_battery_pct();
    // bool charging = power_is_charging();
    // if (pct != last_pct || charging != last_charging) {
    //     last_pct = pct;
    //     last_charging = charging;
    //     ui_update_battery(pct, charging);
    // }

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
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }

    delay(5);
}
