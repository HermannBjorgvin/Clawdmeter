#ifdef PET_ASCII
// No full-size performer in a pet build (this file replaces performer.cpp).
// performer_available() is false, so the director plays its shows as acts in
// the seat and the screensaver's calm dips as a sit in the seat: the path it
// already takes on boards without the memory for the full-size sprite.
#include "../performer.h"

void performer_init(lv_obj_t* parent, int feet_y) { (void)parent; (void)feet_y; }
void performer_tick(void) {}
bool performer_available(void) { return false; }

void performer_place_offstage(int side) { (void)side; }
void performer_walk_home(uint16_t speed_pct) { (void)speed_pct; }
void performer_walk_off(int side, uint16_t speed_pct) { (void)side; (void)speed_pct; }
void performer_run_off(int side, uint16_t from_pct, uint16_t to_pct, uint32_t ramp_ms) {
    (void)side; (void)from_pct; (void)to_pct; (void)ramp_ms;
}
void performer_perform(int anim_index, uint32_t loop_ms) { (void)anim_index; (void)loop_ms; }
void performer_perform_group(int group, uint32_t loop_ms, bool calm) { (void)group; (void)loop_ms; (void)calm; }
void performer_release(void) {}
bool performer_busy(void) { return false; }
bool performer_visible(void) { return false; }
void performer_repaint(void) {}

#endif  // PET_ASCII
