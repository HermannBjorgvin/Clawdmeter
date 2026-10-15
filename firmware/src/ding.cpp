#include "ding.h"
#include <math.h>

static const float TWO_PI_F = 6.28318530718f;

// Tone: C#5 plus faint 3rd, 2nd and 5th partials.
static const float F0 = 554.5f;
static const float P3 = 0.02f, P2 = 0.012f, P5 = 0.012f;
// Envelope: attack, then a slow ring and a fast strike decay.
static const float ATTACK_S = 0.0007f;
static const float SLOW = 0.55f, SLOW_TAU = 0.24f;
static const float FAST = 0.34f, FAST_TAU = 0.009f;
// Click: high-passed noise, gone in a few milliseconds.
static const float CLICK = 0.08f, CLICK_TAU = 0.0015f, CLICK_LEN_S = 0.02f;
// Overall level: +0.8 dB.
static const float LEVEL = 1.0964782f;

// Unit-variance uniform noise for frame `n`, from a counter hash (fixed seed),
// so any frame can be computed on its own and every build makes the same click.
static float noise(uint32_t n) {
    uint32_t h = n * 0x9E3779B9u + 7u;
    h ^= h >> 16; h *= 0x85EBCA6Bu;
    h ^= h >> 13; h *= 0xC2B2AE35u;
    h ^= h >> 16;
    const float u = (float)(h >> 8) * (1.0f / 16777216.0f);   // 0..1
    return (u * 2.0f - 1.0f) * 1.7320508f;
}

uint32_t ding_frames(uint32_t rate) {
    return (uint32_t)(DING_LENGTH_S * (float)rate);
}

float ding_value(uint32_t n, uint32_t rate) {
    if (rate == 0 || n >= ding_frames(rate)) return 0.0f;
    const float t = (float)n / (float)rate;
    const float ph = TWO_PI_F * F0 * t;
    const float tone = sinf(ph) + P3 * sinf(3.0f * ph) + P2 * sinf(2.0f * ph) + P5 * sinf(5.0f * ph);
    float atk = t / ATTACK_S;
    if (atk > 1.0f) atk = 1.0f;
    float y = atk * (SLOW * expf(-t / SLOW_TAU) + FAST * expf(-t / FAST_TAU)) * tone;
    if (t < CLICK_LEN_S) {
        // White noise minus its centred 4-frame mean: a crude high-pass.
        const float mean = (noise(n - 2) + noise(n - 1) + noise(n) + noise(n + 1)) * 0.25f;
        y += CLICK * expf(-t / CLICK_TAU) * (noise(n) - mean);
    }
    y *= LEVEL;
    if (y > 1.0f) y = 1.0f;
    if (y < -1.0f) y = -1.0f;
    return y;
}

int16_t ding_sample(uint32_t n, uint32_t rate, float gain) {
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;
    return (int16_t)lrintf(ding_value(n, rate) * gain * 32767.0f);
}
