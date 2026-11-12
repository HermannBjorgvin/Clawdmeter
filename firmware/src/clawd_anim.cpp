// The only translation unit that includes the generated animation data.
#include "clawd_anim.h"
#include "splash_animations.h"
#include "theme_palette.h"
#include <string.h>

static_assert(SPLASH_PALETTE_SIZE == CLAWD_PALETTE_SIZE, "palette size drifted");

static ClawdAnim table[SPLASH_ANIM_COUNT > 0 ? SPLASH_ANIM_COUNT : 1];
// The art's palettes in the live TH_CLAWD (theme_clawd_art); the frames
// stay in flash.
static uint16_t palettes[SPLASH_ANIM_COUNT > 0 ? SPLASH_ANIM_COUNT : 1][CLAWD_PALETTE_SIZE];
static bool built = false;

static void recolor(void) {
    const uint32_t clawd = theme_hex(TH_CLAWD);
    for (int i = 0; i < SPLASH_ANIM_COUNT; i++)
        for (int c = 0; c < CLAWD_PALETTE_SIZE; c++)
            palettes[i][c] = theme_clawd_art(splash_anims[i].palette[c], clawd);
}

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
        a.palette = palettes[i];
        a.frames = s.frames;
        a.holds = s.holds;
    }
    recolor();
    built = true;
}

void clawd_anim_recolor(void) {
    if (built) recolor();
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
