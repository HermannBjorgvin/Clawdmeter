#include <Arduino.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include "display_cfg_target.h"
#include "data.h"
#include "ui.h"
#include "ble.h"
#include "power.h"
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
#ifndef TARGET_SENSECAP
// Waveshare ESP32-S3-Touch-AMOLED-2.16
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, LCD_RESET, 0 /* rotation */,
    LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 0);
TouchDrvCST92xx touch;
XPowersPMU pmu;
SensorQMI8658 imu;
#else
// SenseCAP Indicator D1L
static Arduino_DataBus *spi_bus_init = new Arduino_ESP32SPI(
    GFX_NOT_DEFINED, GFX_NOT_DEFINED,
    SENSECAP_LCD_SPI_SCK, SENSECAP_LCD_SPI_MOSI, GFX_NOT_DEFINED);
static Arduino_ESP32RGBPanel *rgb_panel = new Arduino_ESP32RGBPanel(
    SENSECAP_LCD_DE, SENSECAP_LCD_VSYNC, SENSECAP_LCD_HSYNC, SENSECAP_LCD_PCLK,
    SENSECAP_LCD_R0, SENSECAP_LCD_R1, SENSECAP_LCD_R2, SENSECAP_LCD_R3, SENSECAP_LCD_R4,
    SENSECAP_LCD_G0, SENSECAP_LCD_G1, SENSECAP_LCD_G2, SENSECAP_LCD_G3, SENSECAP_LCD_G4, SENSECAP_LCD_G5,
    SENSECAP_LCD_B0, SENSECAP_LCD_B1, SENSECAP_LCD_B2, SENSECAP_LCD_B3, SENSECAP_LCD_B4,
    1 /* hsync_polarity */, 10 /* hsync_front_porch */, 8 /* hsync_pulse_width */, 50 /* hsync_back_porch */,
    1 /* vsync_polarity */, 10 /* vsync_front_porch */, 8 /* vsync_pulse_width */, 20 /* vsync_back_porch */,
    0 /* pclk_active_neg */, 12000000 /* prefer_speed */,
    false /* useBigEndian */, 0 /* de_idle_high */, 0 /* pclk_idle_high */,
    480 * 10 /* bounce_buffer_size_px — SRAM relay cuts PSRAM bus contention */);
static Arduino_RGB_Display *gfx_rgb = new Arduino_RGB_Display(
    LCD_WIDTH, LCD_HEIGHT, rgb_panel, 2 /* rotation: panel mounted 180° */, true /* auto_flush */,
    spi_bus_init, GFX_NOT_DEFINED /* RST */,
    st7701_sensecap_init_operations, sizeof(st7701_sensecap_init_operations));
Arduino_GFX *gfx = gfx_rgb;
TouchLib touch_sc(Wire, SENSECAP_IIC_SDA, SENSECAP_IIC_SCL, SENSECAP_TOUCH_ADDR);
PCA9535 pca;
#endif

static UsageData usage = {};

// ---- Touch interrupt + shared state ----
static volatile bool     touch_pressed = false;
static volatile uint16_t touch_x = 0;
static volatile uint16_t touch_y = 0;
static volatile bool     touch_data_ready = false;
#ifdef TARGET_SENSECAP
static bool touch_init_ok = false;
#endif

#ifndef TARGET_SENSECAP
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
#else
static void touch_read() {
    if (!touch_init_ok) return;
    static uint32_t last_ms = 0;
    uint32_t now = millis();
    if (now - last_ms < 20) return;  // poll FT6336 at 50 Hz
    last_ms = now;

    if (touch_sc.read()) {
        TP_Point p = touch_sc.getPoint(0);
        touch_pressed = true;
        // Panel is mounted 180° rotated; flip both axes so LVGL logical (0,0)
        // matches the top-left corner as seen by the user.
        touch_x = (LCD_WIDTH  - 1) - (uint16_t)p.x;
        touch_y = (LCD_HEIGHT - 1) - (uint16_t)p.y;
    } else {
        touch_pressed = false;
    }
}
#endif

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

// LVGL flush callback
static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
#ifdef TARGET_SENSECAP
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)px_map, w, h);
    lv_display_flush_ready(disp);
#else
    // Waveshare: partial strip mode with optional IMU rotation.
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
#endif
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
#ifdef TARGET_SENSECAP
            } else if (strcmp(cmd_buf, "diag") == 0) {
                Serial.printf("touch_init_ok=%d screen=%d touch_pressed=%d x=%d y=%d\n",
                    (int)touch_init_ok, (int)ui_get_current_screen(),
                    (int)touch_pressed, (int)touch_x, (int)touch_y);
                Serial.println("I2C scan:");
                for (uint8_t a = 1; a < 127; a++) {
                    Wire.beginTransmission(a);
                    if (Wire.endTransmission() == 0) {
                        Serial.printf("  found 0x%02X\n", a);
                    }
                }
                Serial.println("I2C scan done");
            } else if (strcmp(cmd_buf, "touch_reset") == 0) {
                // Manually re-run the PCA9535 RST sequence and re-probe.
                touch_init_ok = false;
                Serial.println("Asserting RST low...");
                pca.write(PCA95x5::Port::P07, PCA95x5::Level::L);
                delay(50);
                Serial.println("Releasing RST high...");
                pca.write(PCA95x5::Port::P07, PCA95x5::Level::H);
                delay(500);
                Serial.println("I2C scan after RST release:");
                for (uint8_t a = 1; a < 127; a++) {
                    Wire.beginTransmission(a);
                    if (Wire.endTransmission() == 0) {
                        Serial.printf("  found 0x%02X\n", a);
                    }
                }
                Serial.println("I2C scan done");
                bool ft_found = false;
                // Check for touch IC at expected address
                Wire.beginTransmission(SENSECAP_TOUCH_ADDR);
                ft_found = (Wire.endTransmission() == 0);
                if (ft_found) {
                    touch_init_ok = true;
                    Serial.printf("Touch IC found at 0x%02X — OK\n", SENSECAP_TOUCH_ADDR);
                } else {
                    touch_init_ok = false;
                    Serial.printf("Touch IC not found at 0x%02X\n", SENSECAP_TOUCH_ADDR);
                }
            } else if (strcmp(cmd_buf, "probe48") == 0) {
                // Read a few registers from 0x48 to identify what device it is.
                Serial.println("Probing 0x48:");
                for (uint8_t reg : {0x00u, 0x01u, 0x02u, 0x3Au, 0xA3u, 0xA6u, 0xA8u}) {
                    Wire.beginTransmission(0x48);
                    Wire.write(reg);
                    if (Wire.endTransmission(false) == 0) {
                        Wire.requestFrom((uint8_t)0x48, (uint8_t)1);
                        if (Wire.available()) {
                            uint8_t val = Wire.read();
                            Serial.printf("  reg 0x%02X = 0x%02X (%d)\n", reg, val, val);
                        } else {
                            Serial.printf("  reg 0x%02X = no data\n", reg);
                        }
                    } else {
                        Serial.printf("  reg 0x%02X = nack\n", reg);
                    }
                }
                Serial.println("probe done");
#endif
            }
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

#ifdef TARGET_SENSECAP
static void sensecap_gesture_cb(lv_event_t* e);
#endif

void setup() {
    Serial.begin(115200);
    // Print for 3 s so the serial monitor can be opened before boot messages scroll past.
    delay(300);
    Serial.println("{\"ready\":true}");

#ifndef TARGET_SENSECAP
    // ---- Waveshare: I2C for touch + PMU ----
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
#else
    // ---- SenseCAP: I2C for PCA9535 expander + FT6336 touch ----
    Wire.begin(SENSECAP_IIC_SDA, SENSECAP_IIC_SCL);

    // PCA9535 must be initialised BEFORE gfx->begin().
    // P04 is the SPI CS for the ST7701S init bus (SCK=GPIO41, MOSI=GPIO48).
    // The PCA9535 output register defaults to 0xFFFF at power-on (all HIGH),
    // so P04 starts deasserted.  If we call gfx->begin() without first
    // asserting P04 LOW, every SPI init command is ignored by the ST7701S
    // and the display keeps the RP2040's boot-time settings (COLMOD=0x60
    // RGB666, wrong for our RGB565 pixel pipeline → wrong colours).
    // P07 = touch RST; configure both outputs here in one pass.
    pca.attach(Wire, SENSECAP_PCA9535_ADDR);
    pca.direction(PCA95x5::Port::P04, PCA95x5::Direction::OUT);
    pca.direction(PCA95x5::Port::P07, PCA95x5::Direction::OUT);

    // Give the RP2040 SPI activity time to complete before we assert CS.
    // A USB-only reset (esptool RTS) does not reset the RP2040, so after
    // flashing the RP2040 may still be driving the SPI bus.
    delay(200);

    // Assert LCD SPI CS and send the init sequence (includes COLMOD=0x50).
    pca.write(PCA95x5::Port::P04, PCA95x5::Level::L);
    gfx->begin();
    pca.write(PCA95x5::Port::P04, PCA95x5::Level::H);

    // Turn on backlight
    pinMode(SENSECAP_BACKLIGHT, OUTPUT);
    digitalWrite(SENSECAP_BACKLIGHT, HIGH);

    // Touch RST via P07
    pca.write(PCA95x5::Port::P07, PCA95x5::Level::L);
    delay(50);
    pca.write(PCA95x5::Port::P07, PCA95x5::Level::H);
    delay(300);

    touch_init_ok = touch_sc.init();
    Serial.println(touch_init_ok ? "Touch init OK" : "Touch init failed");
#endif

    // ---- LVGL init (common) ----
    lv_init();
    lv_tick_set_cb(my_tick);

    lv_display_t* disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);

#ifdef TARGET_SENSECAP
    // Full-screen off-screen draw buffer: LCD_WIDTH × LCD_HEIGHT fits the
    // entire frame, so LVGL renders all dirty regions in one pass and calls
    // flush once with the completed composite (background + canvas).
    // The flush callback then copies the finished frame to the display
    // framebuffer in one memcpy — no intermediate states hit the live buffer.
    buf1 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * 2, MALLOC_CAP_SPIRAM);
    lv_display_set_buffers(disp, buf1, NULL, LCD_WIDTH * LCD_HEIGHT * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
#else
    // Waveshare: two PSRAM-backed partial render buffers + rotation buffer.
    buf1 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);
    buf2 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);
    rot_buf = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);
    lv_display_set_buffers(disp, buf1, buf2, LCD_WIDTH * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    // CO5300 requires even-aligned flush regions; ST7701S does not
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);
#endif

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

    // Init BLE data channel
    ble_init();

    // Physical buttons
#ifndef TARGET_SENSECAP
    // Waveshare: back (GPIO 0) and forward (GPIO 18) — BLE HID keystrokes
    pinMode(BTN_BACK, INPUT_PULLUP);
    pinMode(BTN_FWD,  INPUT_PULLUP);
#else
    // SenseCAP: single button cycles screens
    pinMode(SENSECAP_BTN, INPUT_PULLUP);
#endif

    // Build dashboard
    ui_init();

#ifdef TARGET_SENSECAP
    // Register swipe-gesture handler on containers (must be after ui_init so objects exist).
    // lv_layer_top() is not in the parent chain of screen objects, so gesture events from
    // usage_container/ble_container never reach it.  ui_register_sensecap_gesture_cb()
    // sets GESTURE_BUBBLE on each container and attaches the callback to lv_screen_active().
    ui_register_sensecap_gesture_cb(sensecap_gesture_cb);
#endif

    // Show initial BLE status on Bluetooth screen
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());

#ifndef TARGET_SENSECAP
    // Show initial battery status (Waveshare only — SenseCAP hides the widget)
    ui_update_battery(power_battery_pct(), power_is_charging());
#endif

    ui_show_screen(SCREEN_SPLASH);

    Serial.println("Dashboard ready, waiting for data on BLE...");
}

static ble_state_t last_ble_state = BLE_STATE_INIT;

// Brightness ramp state for rotation transition
// On rotation change we blank the panel, force a full LVGL redraw at the
// new orientation, then ramp brightness back up over ~125ms so the
// transition reads as deliberate instead of as a glitch.
// Waveshare only — SenseCAP has no IMU and backlight is a plain GPIO.
#ifndef TARGET_SENSECAP
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

#ifdef TARGET_SENSECAP
// Swipe left → advance screen, swipe right → go back.
// With two main screens (Usage ↔ Bluetooth) either direction cycles identically,
// but directional intent is preserved for if screens are added later.
static void sensecap_gesture_cb(lv_event_t* e) {
    (void)e;
    lv_indev_t* indev = lv_indev_active();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
        if (ui_get_current_screen() == SCREEN_SPLASH) ui_show_screen(SCREEN_USAGE);
        else ui_cycle_screen();
    }
}
#endif

void loop() {
    touch_read();
    lv_timer_handler();
    ui_tick_anim();
    ble_tick();
#ifndef TARGET_SENSECAP
    power_tick();
    imu_tick();
#endif
    splash_tick();

#ifndef TARGET_SENSECAP
    // ---- Waveshare: two buttons + PMU power button ----
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

        if (power_pwr_pressed()) {
            if (ui_get_current_screen() == SCREEN_SPLASH) splash_next();
            else                                          ui_cycle_screen();
        }
    }

    handle_rotation_change();

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
#else
    // ---- SenseCAP: physical button toggles backlight on/off ----
    {
        static bool btn_was      = false;
        static bool backlight_on = true;
        bool btn_now = (digitalRead(SENSECAP_BTN) == LOW);
        if (btn_now && !btn_was) {
            backlight_on = !backlight_on;
            digitalWrite(SENSECAP_BACKLIGHT, backlight_on ? HIGH : LOW);
        }
        btn_was = btn_now;
    }
#endif

    // Update BLE status on screen when state changes
    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
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
#ifdef TARGET_SENSECAP
            if (ui_get_current_screen() == SCREEN_SPLASH) ui_show_screen(SCREEN_USAGE);
#endif
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }

    delay(5);
}
