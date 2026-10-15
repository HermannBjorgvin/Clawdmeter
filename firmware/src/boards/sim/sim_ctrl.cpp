// Controller mode: a host process (tools/simgrid) drives this sim over the
// socket in SIM_CTRL_FD, so many sims (one per panel size) can share one
// clock, one payload stream and one set of inputs.
//
// Time is virtual (arduino_shim.cpp): nothing runs between requests, and
// `advance` runs loop() until the clock has moved on, so a script of
// requests gives the same frames every time.
//
// One request per line; one reply line each ("ok ..." or "err ..."), and
// `frame` follows its line with the pixels:
//   hello                    ok <w> <h> <round> <buttons> <pwr> <minimal>
//   advance <ms>             ok <now_ms> <frame_version>
//   frame                    frame <w> <h> <brightness> <version> <bytes>\n + RGB565 LE
//   payload <json>           ok      (delivered as the app's next BLE write)
//   link <0|1>               ok      (BLE connected / disconnected)
//   cmd <serial line>        ok      (typed into the serial console)
//   touch <x> <y> <0|1>      ok      (finger down at x,y / up)
//   button <boot|secondary|pwr> <0|1>   ok
//   battery <pct> <charging 0|1> <usb_only 0|1>   ok
//   quit                     ok, then exit
#include "board.h"
#include "sim_platform.h"
#include "../../hal/board_caps.h"
#include "../../hal/input_hal.h"
#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

static int ctrl_fd = -2;

bool sim_ctrl_active(void) {
    if (ctrl_fd == -2) {
        const char* v = getenv("SIM_CTRL_FD");
        ctrl_fd = v && *v ? atoi(v) : -1;
    }
    return ctrl_fd >= 0;
}

static bool     touch_down = false;
static uint16_t touch_x = 0, touch_y = 0;
static bool     held[2] = { false, false };   // INPUT_BTN_PRIMARY, INPUT_BTN_SECONDARY

bool sim_ctrl_touch(uint16_t* x, uint16_t* y) {
    *x = touch_x;
    *y = touch_y;
    return touch_down;
}

bool sim_ctrl_button(int btn) { return btn >= 0 && btn < 2 && held[btn]; }

static void write_all(const void* buf, size_t len) {
    const uint8_t* p = (const uint8_t*)buf;
    while (len) {
        const ssize_t n = write(ctrl_fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) exit(0);   // host gone
        p += n;
        len -= (size_t)n;
    }
}

static void reply(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void reply(const char* fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
    line[n++] = '\n';
    write_all(line, (size_t)n);
}

// One request line into `buf` (without the newline); false at EOF.
static bool read_line(char* buf, size_t cap) {
    static char   in[8192];
    static size_t have = 0;
    for (;;) {
        char* nl = (char*)memchr(in, '\n', have);
        if (nl) {
            size_t len = (size_t)(nl - in);
            if (len >= cap) len = cap - 1;
            memcpy(buf, in, len);
            buf[len] = 0;
            const size_t used = (size_t)(nl - in) + 1;
            memmove(in, in + used, have - used);
            have -= used;
            return true;
        }
        if (have == sizeof(in)) have = 0;   // overlong line: drop it
        const ssize_t n = read(ctrl_fd, in + have, sizeof(in) - have);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        have += (size_t)n;
    }
}

static void step_until(unsigned long target) {
    while ((long)(target - millis()) > 0 && !sim_should_quit()) {
        sim_pump();
        loop();
    }
}

static void handle(char* req) {
    char* arg = strchr(req, ' ');
    if (arg) *arg++ = 0;
    else arg = req + strlen(req);

    if (!strcmp(req, "hello")) {
        const BoardCaps& c = board_caps();
        reply("ok %d %d %d %d %d %d", c.width, c.height, c.round_panel, c.button_count,
              c.has_pwr_button, c.minimal_ui);
    } else if (!strcmp(req, "advance")) {
        step_until(millis() + strtoul(arg, NULL, 10));
        uint8_t b;
        uint32_t v;
        sim_display_pixels(&b, &v);
        reply("ok %lu %u", millis(), (unsigned)v);
    } else if (!strcmp(req, "frame")) {
        uint8_t b;
        uint32_t v;
        const uint16_t* px = sim_display_pixels(&b, &v);
        const size_t bytes = (size_t)LCD_WIDTH * LCD_HEIGHT * 2;
        reply("frame %d %d %u %u %zu", LCD_WIDTH, LCD_HEIGHT, b, (unsigned)v, bytes);
        write_all(px, bytes);
    } else if (!strcmp(req, "payload")) {
        sim_ble_push(arg);
        reply("ok");
    } else if (!strcmp(req, "link")) {
        sim_ble_set_link(atoi(arg) != 0);
        reply("ok");
    } else if (!strcmp(req, "cmd")) {
        Serial.feed(arg);
        reply("ok");
    } else if (!strcmp(req, "touch")) {
        int x = 0, y = 0, down = 0;
        sscanf(arg, "%d %d %d", &x, &y, &down);
        touch_x = (uint16_t)(x < 0 ? 0 : x >= LCD_WIDTH ? LCD_WIDTH - 1 : x);
        touch_y = (uint16_t)(y < 0 ? 0 : y >= LCD_HEIGHT ? LCD_HEIGHT - 1 : y);
        touch_down = down != 0;
        reply("ok");
    } else if (!strcmp(req, "button")) {
        char name[16] = "";
        int down = 0;
        sscanf(arg, "%15s %d", name, &down);
        if (!strcmp(name, "boot"))           held[INPUT_BTN_PRIMARY] = down != 0;
        else if (!strcmp(name, "secondary")) held[INPUT_BTN_SECONDARY] = down != 0;
        else if (!strcmp(name, "pwr"))       sim_set_pwr(down != 0);
        else { reply("err unknown button %s", name); return; }
        reply("ok");
    } else if (!strcmp(req, "battery")) {
        int pct = 87, chg = 0, usb = 0;
        sscanf(arg, "%d %d %d", &pct, &chg, &usb);
        sim_set_battery(pct, chg != 0, usb != 0);
        reply("ok");
    } else if (!strcmp(req, "quit")) {
        reply("ok");
        exit(0);
    } else {
        reply("err unknown request %s", req);
    }
}

void sim_ctrl_run(void) {
    setup();
    char req[4096];
    while (read_line(req, sizeof(req))) handle(req);
}
