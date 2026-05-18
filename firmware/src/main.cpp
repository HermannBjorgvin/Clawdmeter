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

// Physical buttons:
//   AMOLED-216: GPIO 0 (Space), GPIO 18 (Shift+Tab), AXP PWR key (cycle screens)
//   LCD-4:      GPIO 0 only — short press = Space, long press ≥700ms = cycle screens
//               (no second user button; GPIO 18 is display R3; RST is hardware-only)
#define BTN_BACK 0
#ifndef BOARD_WAVESHARE_LCD4
#define BTN_FWD  18
#endif

// ---- Hardware objects ----
#ifdef BOARD_WAVESHARE_LCD4
static Arduino_DataBus *lcd4_spi = new Arduino_SWSPI(
    GFX_NOT_DEFINED /* DC */, LCD_SPI_CS,
    LCD_SPI_SCK, LCD_SPI_MOSI, GFX_NOT_DEFINED /* MISO */);
Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
    LCD_DE, LCD_VSYNC, LCD_HSYNC, LCD_PCLK,
    LCD_R0, LCD_R1, LCD_R2, LCD_R3, LCD_R4,
    LCD_G0, LCD_G1, LCD_G2, LCD_G3, LCD_G4, LCD_G5,
    LCD_B0, LCD_B1, LCD_B2, LCD_B3, LCD_B4,
    1 /* hsync_polarity */, 10 /* hsync_front_porch */, 8 /* hsync_pulse_width */, 50 /* hsync_back_porch */,
    1 /* vsync_polarity */, 10 /* vsync_front_porch */, 8 /* vsync_pulse_width */, 20 /* vsync_back_porch */,
    0 /* pclk_active_neg */, GFX_NOT_DEFINED /* prefer_speed */, false /* useBigEndian */,
    0 /* de_idle_high */, 0 /* pclk_idle_high */,
    LCD_WIDTH * 10 /* bounce_buffer_size_px */);
Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
    LCD_WIDTH, LCD_HEIGHT, rgbpanel, 0 /* rotation */,
    true /* auto_flush */,
    lcd4_spi, GFX_NOT_DEFINED /* RST */,
    st7701_type1_init_operations, sizeof(st7701_type1_init_operations));
TouchDrvGT911 touch;
#else
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, LCD_RESET, 0 /* rotation */,
    LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 0);
TouchDrvCST92xx touch;
XPowersPMU pmu;
SensorQMI8658 imu;
#endif

static UsageData usage = {};

// ---- Touch shared state ----
static volatile bool     touch_pressed = false;
static volatile uint16_t touch_x = 0;
static volatile uint16_t touch_y = 0;

#ifndef BOARD_WAVESHARE_LCD4
static volatile bool touch_data_ready = false;

static void IRAM_ATTR touch_isr(void) {
    touch_data_ready = true;
}
#endif

static void touch_read() {
#ifdef BOARD_WAVESHARE_LCD4
    // GT911: poll directly — no dedicated INT pin used
    int16_t tx[5], ty[5];
    uint8_t n = touch.getPoint(tx, ty, touch.getSupportTouchPoint());
    if (n > 0) {
        touch_pressed = true;
        touch_x = (uint16_t)tx[0];
        touch_y = (uint16_t)ty[0];
    } else {
        touch_pressed = false;
    }
#else
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
#endif
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

#ifndef BOARD_WAVESHARE_LCD4
// CO5300 requires even-aligned flush regions
static void rounder_cb(lv_event_t* e) {
    lv_area_t *area = (lv_area_t*)lv_event_get_param(e);
    area->x1 = area->x1 & ~1;
    area->y1 = area->y1 & ~1;
    area->x2 = area->x2 | 1;
    area->y2 = area->y2 | 1;
}
#endif

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

    // Init I2C (shared by touch + PMU/IMU on AMOLED-216)
    Wire.begin(IIC_SDA, IIC_SCL);

#ifdef BOARD_WAVESHARE_LCD4
    // I2C peripheral expander (0x24) — must run before display begin()
    // to enable display power rails and backlight
    Wire.beginTransmission(IO_EXPANDER_ADDR);
    Wire.write(0x02); Wire.write(0xFF);
    Wire.endTransmission();
    Wire.beginTransmission(IO_EXPANDER_ADDR);
    Wire.write(0x03); Wire.write(0x3A);
    Wire.endTransmission();
#endif

    // Init display
    gfx->begin();
    gfx->fillScreen(0x0000);
#ifndef BOARD_WAVESHARE_LCD4
    gfx->setBrightness(200);
#endif

    // Init PMU (AMOLED-216 only; stubs to no-op on LCD-4)
    power_init();

    // Init IMU (AMOLED-216 only; stubs to no-op on LCD-4)
    imu_init();

    // Init touch
#ifdef BOARD_WAVESHARE_LCD4
    // GT911: scan for device at both possible addresses (address set by INT state at reset)
    {
        uint8_t gt_addr = 0x14;
        Wire.beginTransmission(0x5D);
        if (Wire.endTransmission() == 0) gt_addr = 0x5D;
        touch.setPins(-1, -1);
        if (!touch.begin(Wire, gt_addr, IIC_SDA, IIC_SCL)) {
            Serial.println("Touch init failed");
        } else {
            touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
            touch.setSwapXY(false);
            touch.setMirrorXY(false, false);
            Serial.println("Touch init OK");
        }
    }
#else
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
#endif

    // Init LVGL
    lv_init();
    lv_tick_set_cb(my_tick);

    lv_display_t* disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);

#ifdef BOARD_WAVESHARE_LCD4
    // Partial render: 40-line strips keep each draw16bitRGBBitmap call to ~38 KB,
    // which copies in <1 ms and minimises the DMA-race tearing window.
    buf1 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);
    buf2 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);
    lv_display_set_buffers(disp, buf1, buf2, LCD_WIDTH * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
#else
    // AMOLED: partial strips — CO5300 has no continuous DMA race, and the
    // IMU rotation remapping is done per-strip in the flush callback.
    buf1 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);
    buf2 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);
    // rot_buf: holds a rotated strip (480×40 → 40×480, same pixel count)
    rot_buf = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);
    lv_display_set_buffers(disp, buf1, buf2, LCD_WIDTH * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
#endif

#ifndef BOARD_WAVESHARE_LCD4
    // CO5300 even-alignment rounder
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);
#endif

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

    // Init BLE data channel
    ble_init();

    pinMode(BTN_BACK, INPUT_PULLUP);
#ifndef BOARD_WAVESHARE_LCD4
    pinMode(BTN_FWD,  INPUT_PULLUP);
#endif

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
#ifndef BOARD_WAVESHARE_LCD4
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
#endif

void loop() {
    touch_read();
    lv_timer_handler();
    ui_tick_anim();
    ble_tick();
    power_tick();
    imu_tick();
    splash_tick();

    // Button input:
    //   AMOLED-216: BTN_BACK=Space, BTN_FWD=Shift+Tab, AXP PWR=cycle screens
    //   LCD-4:      BTN_BACK only — short press=Space, long press≥700ms=cycle screens
    {
        bool back_now = (digitalRead(BTN_BACK) == LOW);

#ifdef BOARD_WAVESHARE_LCD4
        static bool     back_was        = false;
        static uint32_t back_press_ms   = 0;
        static bool     back_long_fired = false;

        if (back_now != back_was) {
            if (back_now) {
                back_press_ms   = millis();
                back_long_fired = false;
            } else if (!back_long_fired) {
                ble_keyboard_press(0x2C, 0);  // tap Space
                delay(10);
                ble_keyboard_release();
            }
            back_was = back_now;
        }
        if (back_now && !back_long_fired && (millis() - back_press_ms >= 700)) {
            back_long_fired = true;
            if (ui_get_current_screen() == SCREEN_SPLASH) splash_next();
            else                                          ui_cycle_screen();
        }
#else
        static bool back_was = false, fwd_was = false;
        bool fwd_now = (digitalRead(BTN_FWD) == LOW);

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

        if (power_pwr_pressed()) {
            if (ui_get_current_screen() == SCREEN_SPLASH) splash_next();
            else                                          ui_cycle_screen();
        }
#endif
    }

#ifndef BOARD_WAVESHARE_LCD4
    handle_rotation_change();
#endif

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
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }

    delay(5);
}
