#pragma once
#include <stdint.h>

// Read-only view of the official Clawd animations (splash_animations.h).
//
// splash_animations.h defines ~400 KB of `static const` frame data, so it may
// only be included by ONE translation unit (clawd_anim.cpp); every other
// module (performer, corner mascot, director) reaches the art through this view.
//
// Geometry: every frame is a bounding-box crop of the shared 55x37 art stage,
// placed at (ox, oy). All animations end at stage row 37 (feet line), and the
// core poses share one idle-Clawd origin (ox 15), so switching between them
// at the same x is seamless.

#define CLAWD_PALETTE_SIZE 16
#define CLAWD_ART_W        55
#define CLAWD_ART_H        37

struct ClawdAnim {
    const char*     name;
    uint8_t         w, h;              // frame size, cells
    uint8_t         ox, oy;            // crop origin on the 55x37 art stage
    uint16_t        frame_count;
    uint16_t        loop_start, loop_end;
    const uint16_t* palette;           // RGB565, index 0 = background
    const uint8_t*  frames;            // frame_count x (w*h) palette indices
    const uint16_t* holds;             // ms per frame
};

int              clawd_anim_count(void);
const ClawdAnim* clawd_anim_at(int index);          // NULL when out of range
const ClawdAnim* clawd_anim(const char* name);      // NULL when not found
int              clawd_anim_index(const char* name); // -1 when not found

static inline const uint8_t* clawd_frame(const ClawdAnim* a, uint16_t frame) {
    return &a->frames[(uint32_t)frame * a->w * a->h];
}

// ─── Walk gait ───────────────────────────────────────────────────────────────
// The walk gaits animate in place; screen travel is ours, locked to the feet:
// per-frame movement equals the measured backward drift of the planted feet.
//   walking      (5-frame waddle loop [2..6]): 1,1,1,1,2 cells -> 6 cells / 450 ms
//   crab walking (8-frame scuttle loop [1..8]): surges entering 4, 5, 8 and
//                the cycle wrap -> 4 cells / 640 ms
// `from_loop` = the previous frame was already inside the gait loop.
enum ClawdGait { GAIT_NONE, GAIT_WALK, GAIT_CRAB };

static inline int clawd_gait_cells(ClawdGait g, uint16_t frame, bool from_loop) {
    if (g == GAIT_CRAB) {
        if (frame == 1) return from_loop ? 1 : 0;
        return (frame == 4 || frame == 5 || frame == 8) ? 1 : 0;
    }
    if (g == GAIT_WALK) {
        if (frame < 2 || frame > 6) return 0;      // idle / wind-up / outro
        if (frame == 2 && !from_loop) return 0;    // first plant
        return (frame == 6) ? 2 : 1;
    }
    return 0;
}

// Frame hold scaled by a speed in percent (100 = authored timing, 200 = 2x).
static inline uint32_t clawd_hold(const ClawdAnim* a, uint16_t frame, uint16_t speed_pct) {
    if (speed_pct == 0) speed_pct = 100;
    return (uint32_t)a->holds[frame] * 100u / speed_pct;
}
