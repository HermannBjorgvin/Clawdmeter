#pragma once
#include <stdint.h>
#include <lvgl.h>

// The stage room: full-screen 60x60-cell pixel stage where Clawd performs the
// official scenes (8 px cells on a 480 panel). The director
// (clawd_director.cpp) decides when the stage is open and what the performer
// does; nothing here starts on its own.
//
// Performer model: one Clawd with an x offset (cells) from his authored home
// spot. He is offstage, standing (the official idle pose), walking, or
// performing a scene. Scenes always end on the idle pose, so the next thing
// starts from the same spot.

void splash_init(lv_obj_t *parent);
void splash_tick(void);

// Room
void splash_open(void);           // show the stage (performer keeps his state)
void splash_close(void);          // hide; performer goes offstage, clip reset
bool splash_is_active(void);
lv_obj_t* splash_get_root(void);
// Boards without PSRAM draw the stage straight to the panel, bypassing LVGL.
// While an LVGL overlay (face, streak) covers it, panel writes pause; the
// stage repaints in full once it's uncovered.
void splash_set_occluded(bool occluded);

// Performer
void splash_place_home(void);                 // standing at home, idle pose
void splash_place_offstage_right(void);
void splash_walk_home(uint16_t speed_pct);    // walking gait to home, then stand
void splash_walk_off_right(uint16_t speed_pct);
// A run-off: the walk off right starts at from_pct and speeds up to to_pct
// over ramp_ms (ease-in), so he gets going instead of starting at full tilt.
void splash_run_off_right(uint16_t from_pct, uint16_t to_pct, uint32_t ramp_ms);
// Perform one scene at home, holding its loop for loop_ms, then its outro;
// ends standing. Walker scenes (walking, crab walking) do one round trip.
void splash_perform(int anim_index, uint32_t loop_ms);
// Pick a scene from mood group 0..3 (never the one before), perform it.
// calm = only the gentle Idle-group scenes (magnifier, walking). Never the
// same scene twice running (tracked separately for shows and calm dips).
void splash_perform_group(int group, uint32_t loop_ms, bool calm);
void splash_release(void);    // tap: stop now, snap to the idle pose where he is
bool splash_busy(void);       // walking or performing
bool splash_performer_visible(void);   // any of him is on the stage right now
void splash_next(void);       // PWR on the stage: the next scene in the group (calm stays calm)
