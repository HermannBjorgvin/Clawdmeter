#pragma once

#include <stdint.h>

// Beeps for boards whose buzzer is a plain on/off switch (an active buzzer:
// fixed pitch, no volume). It can't play the ding (ding.h), so a chime is
// three short beeps instead, and the boot ding (chime.cpp) is one. The
// board's sound.cpp owns the pin and passes in how to switch it; this module
// only keeps the timing, without blocking.
//
// The set function returns false when the write failed. A failed "off" drops
// the rest of the pattern and is retried every BUZZER_OFF_RETRY_MS until it
// lands: a buzzer left on is a loud continuous tone. A failed "on" just
// carries on with the pattern.
//
// Arduino-free (the caller passes the time) so it runs in the native tests.

#define BUZZER_BEEPS        3    // a chime: alerts, session resets, `buzz`
#define BUZZER_BOOT_BEEPS   1    // in place of the speaker boards' boot ding
#define BUZZER_ON_MS        90
#define BUZZER_GAP_MS       110
#define BUZZER_OFF_RETRY_MS 200

typedef bool (*buzzer_set_fn)(bool on);

// Switches the buzzer off and queues the boot beep (BUZZER_BOOT_BEEPS), which
// plays from the first tick, once setup() is done, as the boot ding does.
// A failed "off" skips it. Call from sound_hal_init().
void buzzer_init(buzzer_set_fn set, uint32_t now_ms);
// Queues `beeps` beeps (BUZZER_BEEPS for a chime). Ignored while a pattern
// or an "off" retry is pending, so a second chime never cuts the first short.
void buzzer_start(uint32_t now_ms, int beeps);
// Makes any edge that is due. Call every loop from sound_hal_tick().
void buzzer_tick(uint32_t now_ms);
// A pattern or an "off" retry is still pending.
bool buzzer_busy(void);
