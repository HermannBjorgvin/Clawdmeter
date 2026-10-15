#include "perf.h"
#include <Arduino.h>

static const char* const NAMES[PERF_WINDOW_COUNT] = { "face-rise", "face-settle", "face-drop", "streak" };

struct PerfStats {
    bool     done;          // a complete (or cut-short) run is recorded
    bool     cut;           // another window started before this one ended
    uint32_t start_ms, dur_ms;
    uint32_t frames;
    uint64_t refr_us, flush_us, px;
    uint32_t refr_max_us;
    uint32_t gap_max_ms;    // longest wait between two frames (or from the start)
};

static PerfStats stats[PERF_WINDOW_COUNT];
static int       open_w = -1;
static uint32_t  last_frame_ms = 0;
static uint32_t  refr_t0 = 0, flush_t0 = 0;
static uint32_t  frame_px = 0, frame_flush_us = 0;
static uint32_t  panel_px = 1;
static uint32_t  period_ms = 0;

static void on_event(lv_event_t* e) {
    if (open_w < 0) return;
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_REFR_START) {
        refr_t0 = micros();
        frame_px = 0;
        frame_flush_us = 0;
    } else if (code == LV_EVENT_FLUSH_START) {
        const lv_area_t* a = (const lv_area_t*)lv_event_get_param(e);
        frame_px += lv_area_get_size(a);
        flush_t0 = micros();
    } else if (code == LV_EVENT_FLUSH_FINISH) {
        frame_flush_us += micros() - flush_t0;
    } else if (code == LV_EVENT_REFR_READY) {
        if (!frame_px) return;                  // nothing was invalid
        PerfStats& s = stats[open_w];
        const uint32_t us = micros() - refr_t0;
        const uint32_t now = millis();
        s.frames++;
        s.refr_us += us;
        s.flush_us += frame_flush_us;
        s.px += frame_px;
        if (us > s.refr_max_us) s.refr_max_us = us;
        if (now - last_frame_ms > s.gap_max_ms) s.gap_max_ms = now - last_frame_ms;
        last_frame_ms = now;
    }
}

void perf_attach(lv_display_t* disp, uint32_t refr_period_ms) {
    period_ms = refr_period_ms;
    panel_px = (uint32_t)lv_display_get_horizontal_resolution(disp) *
               (uint32_t)lv_display_get_vertical_resolution(disp);
    if (!panel_px) panel_px = 1;
    lv_display_add_event_cb(disp, on_event, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(disp, on_event, LV_EVENT_FLUSH_START, NULL);
    lv_display_add_event_cb(disp, on_event, LV_EVENT_FLUSH_FINISH, NULL);
    lv_display_add_event_cb(disp, on_event, LV_EVENT_REFR_READY, NULL);
}

static void close_open(bool cut) {
    if (open_w < 0) return;
    PerfStats& s = stats[open_w];
    s.dur_ms = millis() - s.start_ms;
    s.done = true;
    s.cut = cut;
    open_w = -1;
}

void perf_begin(PerfWindow w) {
    close_open(true);
    PerfStats& s = stats[w];
    s = PerfStats();
    s.start_ms = millis();
    last_frame_ms = s.start_ms;
    open_w = w;
}

void perf_end(PerfWindow w) {
    if (open_w == (int)w) close_open(false);
}

void perf_print(void) {
    Serial.printf("perf: refresh period %lu ms\n", (unsigned long)period_ms);
    for (int i = 0; i < PERF_WINDOW_COUNT; i++) {
        const PerfStats& s = stats[i];
        if (!s.done) { Serial.printf("perf %s: no run yet\n", NAMES[i]); continue; }
        const uint32_t f = s.frames ? s.frames : 1;
        Serial.printf("perf %s: %lu ms%s, %lu frames (%.1f fps), longest gap %lu ms; "
                      "refresh avg %.1f ms (max %.1f), flush avg %.1f ms; "
                      "%lu px/frame (%.1f%% of the panel)\n",
                      NAMES[i], (unsigned long)s.dur_ms, s.cut ? " (cut short)" : "",
                      (unsigned long)s.frames,
                      s.dur_ms ? s.frames * 1000.0f / s.dur_ms : 0.0f,
                      (unsigned long)s.gap_max_ms,
                      s.refr_us / 1000.0f / f, s.refr_max_us / 1000.0f,
                      s.flush_us / 1000.0f / f,
                      (unsigned long)(s.px / f), 100.0f * (float)(s.px / f) / panel_px);
    }
}
