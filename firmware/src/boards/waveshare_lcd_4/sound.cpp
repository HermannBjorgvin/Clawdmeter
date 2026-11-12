#include "../../hal/sound_hal.h"
#include "io_expander.h"
#include <Arduino.h>

// Active buzzer on the expander's BEE_EN pin: HIGH sounds a fixed tone, so a
// "note" is just an on/off span. sound_hal_tick() walks the pattern without
// blocking. Each write is one I2C attempt. An "off" that fails drops the rest
// of the pattern and is retried every OFF_RETRY_MS until it lands: a
// latched-on buzzer is a loud continuous tone until power cycle.
//
// The buzzer has a fixed pitch and no volume, so the shared ding (ding.h) is
// not used here: sound_hal_chime() plays three short beeps instead (alerts
// and session resets). sound_hal_tune() has nothing to tune.

#define BEEPS_PER_CHIME 3
#define BEEP_ON_MS      90
#define BEEP_GAP_MS     110
#define OFF_RETRY_MS    200

static int      edges_left  = 0;     // on/off transitions still to do
static bool     buzzing     = false;
static bool     off_pending = false;
static uint32_t next_ms     = 0;     // next edge, or next "off" retry

static void buzzer_off_failed(void) {
    off_pending = true;
    edges_left  = 0;
    buzzing     = false;
    next_ms     = millis() + OFF_RETRY_MS;
}

void sound_hal_init(void) {
    if (!io_expander_set_buzzer(false)) buzzer_off_failed();
}

void sound_hal_tick(void) {
    if ((int32_t)(millis() - next_ms) < 0) return;
    if (off_pending) {
        if (io_expander_set_buzzer(false)) off_pending = false;
        else                               next_ms = millis() + OFF_RETRY_MS;
        return;
    }
    if (edges_left == 0) return;
    buzzing = !buzzing;
    edges_left--;
    if (!io_expander_set_buzzer(buzzing) && !buzzing) {
        buzzer_off_failed();
        return;
    }
    next_ms = millis() + (buzzing ? BEEP_ON_MS : BEEP_GAP_MS);
}

void sound_hal_chime(void) {
    if (edges_left > 0 || off_pending) return;   // don't cut a pattern short
    buzzing    = false;
    edges_left = BEEPS_PER_CHIME * 2;
    next_ms    = millis();
}

void sound_hal_tune(float gain, int volume) { (void)gain; (void)volume; }
