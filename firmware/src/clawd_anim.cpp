// The only translation unit that includes the generated animation data.
#include "clawd_anim.h"
#include "splash_animations.h"
#include <string.h>

static_assert(SPLASH_PALETTE_SIZE == CLAWD_PALETTE_SIZE, "palette size drifted");

static ClawdAnim table[SPLASH_ANIM_COUNT > 0 ? SPLASH_ANIM_COUNT : 1];
static bool built = false;

static void build(void) {
    if (built) return;
    for (int i = 0; i < SPLASH_ANIM_COUNT; i++) {
        const splash_anim_def_t& s = splash_anims[i];
        ClawdAnim& a = table[i];
        a.name = s.name;
        a.w = s.w;  a.h = s.h;
        a.ox = s.ox; a.oy = s.oy;
        a.frame_count = s.frame_count;
        a.loop_start = s.loop_start;
        a.loop_end = s.loop_end;
        a.palette = s.palette;
        a.frames = s.frames;
        a.holds = s.holds;
    }
    built = true;
}

int clawd_anim_count(void) { return SPLASH_ANIM_COUNT; }

const ClawdAnim* clawd_anim_at(int index) {
    if (index < 0 || index >= SPLASH_ANIM_COUNT) return NULL;
    build();
    return &table[index];
}

int clawd_anim_index(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < SPLASH_ANIM_COUNT; i++)
        if (strcmp(splash_anims[i].name, name) == 0) return i;
    return -1;
}

const ClawdAnim* clawd_anim(const char* name) {
    return clawd_anim_at(clawd_anim_index(name));
}
