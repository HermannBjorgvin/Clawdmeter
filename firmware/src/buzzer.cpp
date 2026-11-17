#include "buzzer.h"

static buzzer_set_fn set_fn      = nullptr;
static int           edges_left  = 0;      // on/off transitions still to do
static bool          buzzing     = false;
static bool          off_pending = false;
static uint32_t      next_ms     = 0;      // next edge, or next "off" retry

static void off_failed(uint32_t now_ms) {
    off_pending = true;
    edges_left  = 0;
    buzzing     = false;
    next_ms     = now_ms + BUZZER_OFF_RETRY_MS;
}

void buzzer_init(buzzer_set_fn set, uint32_t now_ms) {
    set_fn      = set;
    edges_left  = 0;
    buzzing     = false;
    off_pending = false;
    next_ms     = now_ms;
    if (!set_fn) return;
    if (!set_fn(false)) off_failed(now_ms);
    else                buzzer_start(now_ms, BUZZER_BOOT_BEEPS);
}

void buzzer_start(uint32_t now_ms, int beeps) {
    if (!set_fn || beeps <= 0 || buzzer_busy()) return;
    buzzing    = false;
    edges_left = beeps * 2;
    next_ms    = now_ms;
}

void buzzer_tick(uint32_t now_ms) {
    if (!set_fn || (int32_t)(now_ms - next_ms) < 0) return;
    if (off_pending) {
        if (set_fn(false)) off_pending = false;
        else               next_ms = now_ms + BUZZER_OFF_RETRY_MS;
        return;
    }
    if (edges_left == 0) return;
    buzzing = !buzzing;
    edges_left--;
    if (!set_fn(buzzing) && !buzzing) {
        off_failed(now_ms);
        return;
    }
    next_ms = now_ms + (buzzing ? BUZZER_ON_MS : BUZZER_GAP_MS);
}

bool buzzer_busy(void) {
    return edges_left > 0 || off_pending;
}
