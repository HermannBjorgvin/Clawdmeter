#pragma once
#include <stdint.h>

// The face's rise and drop as a pure function of elapsed time
// (docs/clawd-director.md, "The face's motion"). No LVGL, no Arduino: the
// same (move, t, geometry) always gives the same pose, so it's unit-tested on
// the host and the durations never depend on frame rate.
//
// Hand-keyed, not simulated, and strictly vertical: the top edge is always
// flat and the eyes level (a tilt would have to be drawn as a stepped shear,
// which aliases badly on the panel). The rise is an ease-out past rest, then
// an underdamped spring settle, with the arms out on a narrower body that
// widens to full bleed as he presses to the glass. The drop stays full width:
// an anticipation crouch and hop, then an ease-in fall. Eyes and arms lag the
// body; the eyes squash and stretch.

enum FaceMove { FACE_MOVE_RISE, FACE_MOVE_DROP };

struct FaceGeom {
    int w, h;          // panel
    int mind;          // shorter side: the design scale (authored on 480)
    int cell;          // art cell (min(W,H)/60): the arms and the widening step in whole cells
    int start_y;       // where the body's top edge starts (h = from below the bezel)
};

struct FacePose {
    int y;             // the body's top edge
    int margin;        // px the body is narrower than the panel on each side (arms show there)
    int eye_dy;        // eyes' follow-through, px, on top of riding the body
    int arm_dy;        // arms' follow-through, px
    int stretch;       // eyes, permille: +200 = 20 % taller and narrower, -200 squashed
};

uint32_t face_motion_duration(FaceMove m);
// When the rise first reaches rest (the end of `face-rise` in perf; the
// settle runs from there to the end).
uint32_t face_motion_rest_ms(void);
FacePose face_motion_pose(FaceMove m, uint32_t t_ms, const FaceGeom* g);

// Arm nub size (also the body's narrowing while it rises), px.
int face_arm_w(const FaceGeom* g);
int face_arm_h(const FaceGeom* g);
