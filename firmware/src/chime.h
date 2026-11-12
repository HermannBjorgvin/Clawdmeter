#pragma once
#include <stdint.h>

// Board-agnostic ES8311 + I2S chime engine. A board's sound.cpp fills in a
// ChimeConfig (its I2S pins, codec address, volume, and a power-amp enable
// hook) and calls chime_init() once; the ding (ding.h) is rendered in the
// background after setup and streamed to the codec by a one-shot FreeRTOS
// task, so the LVGL loop never blocks. It plays once at boot. Any sample rate
// the ES8311 table has works; 16-bit stereo out.
//
// amp_enable is the only part that differs structurally between boards: the
// 2.16 toggles a GPIO, the 1.8 drives an XCA9554 expander line. May be null
// (an amp with no enable line): the codec's DAC is then muted between dings.

typedef void (*amp_enable_fn)(bool on);

struct ChimeConfig {
    int          mclk, bclk, ws, dout, din;  // I2S GPIOs
    int          sample_rate;                // I2S rate the ding is synthesized at
    uint8_t      es8311_addr;                // codec I2C address (0x18)
    uint8_t      volume;                     // 0..100
    amp_enable_fn amp_enable;                // power-amp on/off hook (nullable)
};

// Bring up I2S + the ES8311 codec. Returns true on success; false leaves the
// engine inert (chime_play becomes a no-op). Wire/I2C must already be up.
bool chime_init(const ChimeConfig& cfg);

// Queue one ding (non-blocking). No-op if not ready or already playing;
// before the render is done, it plays when the render finishes.
void chime_play(void);

// Call every loop: the first call starts the background render.
void chime_tick(void);

// Speaker tuning: set the ding's gain (0..1 of its own level) and, when volume >= 0, the
// codec volume (0..100) until reboot, then play it (after a background
// re-render when the gain changed).
void chime_tune(float gain, int volume);
