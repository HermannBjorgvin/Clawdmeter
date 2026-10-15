// Host unit test for face_motion — the face's rise and drop as a pure
// function of time (docs/clawd-director.md, "The face's motion"):
//
//   pio test -d firmware -e native -f test_face_motion

#include "face_motion.h"
#include <unity.h>
#include <stdlib.h>

#define CHECK(cond) TEST_ASSERT_TRUE(cond)

void setUp(void) {}
void tearDown(void) {}

static FaceGeom geom(int w, int h, int start_y) {
    FaceGeom g;
    g.w = w; g.h = h;
    g.mind = w < h ? w : h;
    g.cell = g.mind / 60;
    g.start_y = start_y;
    return g;
}

static const int SIZES[][2] = { { 480, 480 }, { 368, 448 }, { 410, 502 }, { 240, 240 } };

static void rise_starts_below_and_ends_dead_still_at_rest() {
    for (const auto& s : SIZES) {
        FaceGeom g = geom(s[0], s[1], s[1]);
        const FacePose p0 = face_motion_pose(FACE_MOVE_RISE, 0, &g);
        CHECK(p0.y == s[1]);
        const FacePose e = face_motion_pose(FACE_MOVE_RISE, face_motion_duration(FACE_MOVE_RISE), &g);
        CHECK(e.y == 0 && e.margin == 0);
        CHECK(e.eye_dy == 0 && e.arm_dy == 0 && e.stretch == 0);
    }
}

static void rise_overshoots_then_dips_then_settles() {
    FaceGeom g = geom(480, 480, 480);
    int min_y = 0, max_after = 0;
    for (uint32_t t = 0; t <= face_motion_duration(FACE_MOVE_RISE); t += 4) {
        const FacePose p = face_motion_pose(FACE_MOVE_RISE, t, &g);
        if (p.y < min_y) min_y = p.y;
        if (t > 520 && p.y > max_after) max_after = p.y;
    }
    CHECK(min_y <= -30 && min_y >= -60);     // past rest, not wildly
    CHECK(max_after >= 6 && max_after <= 20); // a small dip back
    CHECK(face_motion_rest_ms() >= 560 && face_motion_rest_ms() <= 620);
    CHECK(face_motion_pose(FACE_MOVE_RISE, face_motion_rest_ms() - 20, &g).y < 0);
    CHECK(face_motion_pose(FACE_MOVE_RISE, face_motion_rest_ms() + 20, &g).y > 0);
}

static void the_face_keeps_one_size() {
    for (const auto& s : SIZES) {
        FaceGeom g = geom(s[0], s[1], s[1]);
        const FaceMove moves[] = {FACE_MOVE_RISE, FACE_MOVE_DROP};
        for (FaceMove m : moves)
            for (uint32_t t = 0; t <= face_motion_duration(m); t += 2) {
                const FacePose p = face_motion_pose(m, t, &g);
                CHECK(p.margin == 0 && p.arm_dy == 0 && p.stretch == 0);
            }
    }
}

static void drop_stays_full_width_the_whole_way() {
    for (const auto& s : SIZES) {
        FaceGeom g = geom(s[0], s[1], 0);
        for (uint32_t t = 0; t <= face_motion_duration(FACE_MOVE_DROP); t += 2) {
            const FacePose p = face_motion_pose(FACE_MOVE_DROP, t, &g);
            CHECK(p.margin == 0);            // no narrowing, no arms popping in
            CHECK(p.arm_dy == 0);
        }
    }
}

static void drop_anticipates_then_falls_below_the_bezel() {
    for (const auto& s : SIZES) {
        FaceGeom g = geom(s[0], s[1], 0);
        // A crouch (down), then a hop up.
        int max_y = 0, min_y = 0;
        for (uint32_t t = 0; t < 150; t += 2) {
            const FacePose p = face_motion_pose(FACE_MOVE_DROP, t, &g);
            if (p.y > max_y) max_y = p.y;
            if (p.y < min_y) min_y = p.y;
        }
        CHECK(max_y > 0 && min_y < 0);
        // Then an ease-in fall: it only goes down, faster and faster.
        int prev = face_motion_pose(FACE_MOVE_DROP, 150, &g).y, prev_dy = 0;
        for (uint32_t t = 170; t <= face_motion_duration(FACE_MOVE_DROP); t += 20) {
            const int y = face_motion_pose(FACE_MOVE_DROP, t, &g).y;
            CHECK(y > prev);
            CHECK(y - prev >= prev_dy - 1);
            prev_dy = y - prev;
            prev = y;
        }
        const FacePose e = face_motion_pose(FACE_MOVE_DROP, face_motion_duration(FACE_MOVE_DROP), &g);
        CHECK(e.y >= s[1]);
        // The eyes lag a little, vertically, but never leave the body.
        CHECK(e.eye_dy <= 0 && e.eye_dy >= -40 * g.mind / 480);
    }
}

static void deterministic() {
    FaceGeom g = geom(368, 448, 448);
    for (uint32_t t = 0; t < 900; t += 7) {
        const FacePose a = face_motion_pose(FACE_MOVE_RISE, t, &g);
        const FacePose b = face_motion_pose(FACE_MOVE_RISE, t, &g);
        CHECK(a.y == b.y && a.margin == b.margin && a.stretch == b.stretch && a.eye_dy == b.eye_dy);
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(rise_starts_below_and_ends_dead_still_at_rest);
    RUN_TEST(rise_overshoots_then_dips_then_settles);
    RUN_TEST(the_face_keeps_one_size);
    RUN_TEST(drop_stays_full_width_the_whole_way);
    RUN_TEST(drop_anticipates_then_falls_below_the_bezel);
    RUN_TEST(deterministic);
    return UNITY_END();
}
