#include "face_motion.h"
#include <math.h>

// All distances are authored on a 480 px panel and scaled by the shorter side;
// times are absolute ms. See docs/clawd-director.md, "The face's motion".

// Rise: ease-out to OVER past rest, then a spring settle.
#define RISE_UP_MS      500     // ease-out leg, ends at the overshoot apex
#define RISE_END_MS     860     // settle done: dead still at rest
#define RISE_OVER       44      // overshoot past rest, px@480
#define SPRING_MS       300.0f  // settle period
#define SPRING_TAU      110.0f  // settle decay

// Drop: crouch, hop, ease-in fall.
#define DROP_CROUCH_MS  60
#define DROP_HOP_MS     90
#define DROP_ANT_MS     (DROP_CROUCH_MS + DROP_HOP_MS)
#define DROP_FALL_MS    360
#define DROP_END_MS     (DROP_ANT_MS + DROP_FALL_MS)
#define DROP_CROUCH     6       // px@480: the crouch before the hop
#define DROP_HOP        28      // px@480

// Follow-through: the eyes trail the body.
#define EYE_LAG_MS      40
#define EYE_LAG_MAX     40      // px@480

#define ARM_W           48      // px@480: 6 cells wide, 7 tall
#define ARM_H           56

static const float PI_F = 3.14159265f;

static int sc(const FaceGeom* g, int v) { return v * g->mind / 480; }
static float scf(const FaceGeom* g, float v) { return v * (float)g->mind / 480.0f; }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int iround(float v) { return (int)(v < 0 ? v - 0.5f : v + 0.5f); }

static int cell_of(const FaceGeom* g) { return g->cell > 0 ? g->cell : 1; }

int face_arm_w(const FaceGeom* g) {
    const int c = cell_of(g);
    const int w = sc(g, ARM_W) / c * c;
    return w > c ? w : c;
}

int face_arm_h(const FaceGeom* g) {
    const int c = cell_of(g);
    const int h = sc(g, ARM_H) / c * c;
    return h > c ? h : c;
}

uint32_t face_motion_duration(FaceMove m) {
    return m == FACE_MOVE_RISE ? RISE_END_MS : DROP_END_MS;
}

// An underdamped spring released at rest-offset 1 with zero velocity:
// 1 at t = 0, decaying oscillation about 0.
static float spring(float t, float period, float tau) {
    const float w = 2.0f * PI_F / period;
    return expf(-t / tau) * (cosf(w * t) + sinf(w * t) / (w * tau));
}

uint32_t face_motion_rest_ms(void) {
    // First zero of spring(): tan(wt) = -w*tau.
    const float w = 2.0f * PI_F / SPRING_MS;
    const float t = (PI_F - atanf(w * SPRING_TAU)) / w;
    return RISE_UP_MS + (uint32_t)(t + 0.5f);
}

static int drop_end_y(const FaceGeom* g) {
    return g->h + cell_of(g);      // just past the bezel
}

static float body_y(FaceMove m, float t, const FaceGeom* g) {
    if (t < 0) t = 0;
    if (m == FACE_MOVE_RISE) {
        const float over = scf(g, RISE_OVER);
        if (t < RISE_UP_MS) {
            const float u = t / RISE_UP_MS;
            const float e = 1.0f - (1.0f - u) * (1.0f - u);
            return g->start_y - (g->start_y + over) * e;
        }
        if (t >= RISE_END_MS) return 0;
        return -over * spring(t - RISE_UP_MS, SPRING_MS, SPRING_TAU);
    }
    if (t < DROP_CROUCH_MS)
        return g->start_y + scf(g, DROP_CROUCH) * sinf(PI_F * t / DROP_CROUCH_MS);
    const float top = g->start_y - scf(g, DROP_HOP);
    if (t < DROP_ANT_MS) {
        const float v = (t - DROP_CROUCH_MS) / DROP_HOP_MS;
        return g->start_y - scf(g, DROP_HOP) * (1.0f - (1.0f - v) * (1.0f - v));
    }
    if (t >= DROP_END_MS) return drop_end_y(g);
    const float w = (t - DROP_ANT_MS) / DROP_FALL_MS;
    return top + (drop_end_y(g) - top) * powf(w, 2.0f);
}

FacePose face_motion_pose(FaceMove m, uint32_t t_ms, const FaceGeom* g) {
    FacePose p = {};
    const float t = (float)t_ms;
    const float y = body_y(m, t, g);
    p.y = iround(y);
    p.eye_dy = clampi(iround(body_y(m, t - EYE_LAG_MS, g) - y), -sc(g, EYE_LAG_MAX), sc(g, EYE_LAG_MAX));

    // The face keeps one size: no narrowing, no squash and stretch. Only its
    // position moves, with the eyes trailing the body.
    if (m == FACE_MOVE_RISE && t_ms >= RISE_END_MS) { p.y = 0; p.eye_dy = 0; }
    return p;
}
