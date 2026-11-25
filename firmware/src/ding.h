#pragma once
#include <stdint.h>

// The one sound Clawdmeter makes (attention alert and session-reset chime):
// a ding at C#5. A sine strike with three faint partials, a 0.7 ms attack,
// a two-stage decay (a fast 9 ms fall into a slow 0.24 s ring) and a short
// high-passed noise click at the start, synthesized here so no audio file
// ships. The desktop app plays the same ding (clawdmeter-app/src/chime.rs);
// keep the two in step.
//
// Pure math, no Arduino: native-tested in test/test_ding.

#define DING_LENGTH_S   1.67f     // the ring is ~-65 dB by then

// Frames in one ding at `rate` Hz.
uint32_t ding_frames(uint32_t rate);

// Frame `n` of the ding at `rate` Hz, in -1..1 (0 past the end). Peaks at
// about -0.6 dBFS.
float ding_value(uint32_t n, uint32_t rate);

// Frame `n` as a 16-bit sample at `gain` (0..1 of the ding's own level).
int16_t ding_sample(uint32_t n, uint32_t rate, float gain);

// A softer envelope of the same C#5, for boards whose small speaker turns the
// strike and click into a crack: a 15 ms raised-cosine attack, no strike, no
// click, the 2nd and 3rd partials only, one 0.32 s ring and a 0.25 s
// raised-cosine fade to silence at the end. Same length, about the same peak.
// Firmware only (the desktop plays DING_STRIKE); a board picks it in its
// ChimeConfig.
enum DingStyle { DING_STRIKE = 0, DING_SOFT = 1 };

float   ding_value_style(uint32_t n, uint32_t rate, DingStyle style);
int16_t ding_sample_style(uint32_t n, uint32_t rate, float gain, DingStyle style);
