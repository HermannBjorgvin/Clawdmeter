#ifdef PET_ASCII
// Species dispatch and the drawing helpers the species files call, ported
// from claude-desktop-buddy's buddy.cpp (MIT, see LICENSE-buddy). There the
// helpers drew through Arduino_GFX; here they draw the same 6x8 text cells
// into a plain RGB565 buffer, so this file builds in the simulator too.
#include "pet_buddy.h"
#include "buddy.h"
#include "buddy_common.h"
#include "pet_font.h"
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

// ──────────────── shared geometry (buddy_common.h) ────────────────
const int BUDDY_X_CENTER  = PET_W / 2;
const int BUDDY_CANVAS_W  = PET_W;
const int BUDDY_Y_BASE    = PET_BODY_TOP;
const int BUDDY_Y_OVERLAY = 6;
const int BUDDY_CHAR_W    = 6;
const int BUDDY_CHAR_H    = 8;

// ──────────────── shared colors ────────────────
const uint16_t BUDDY_BG     = 0x0000;
const uint16_t BUDDY_HEART  = 0xF810;
const uint16_t BUDDY_DIM    = 0x8410;
const uint16_t BUDDY_YEL    = 0xFFE0;
const uint16_t BUDDY_WHITE  = 0xFFFF;
const uint16_t BUDDY_CYAN   = 0x07FF;
const uint16_t BUDDY_GREEN  = 0x07E0;
const uint16_t BUDDY_PURPLE = 0xA01F;
const uint16_t BUDDY_RED    = 0xF800;
const uint16_t BUDDY_BLUE   = 0x041F;

// ──────────────── the canvas and its text cursor ────────────────
static uint16_t canvas[PET_W * PET_H];
static int      cur_x = 0, cur_y = 0;
static uint16_t cur_fg = 0xFFFF;

// One 6x8 character cell at (x, y), clipped to the canvas. Like the GFX text
// it replaces, the whole cell is painted: glyph pixels in `fg`, the rest in
// the background, so a later print erases what was under it.
static void put_char(int x, int y, char ch, uint16_t fg) {
    const uint8_t c = (uint8_t)ch;
    const uint8_t* g = PET_FONT[(c >= 0x20 && c <= 0x7E) ? c - 0x20 : '?' - 0x20];
    for (int col = 0; col < 6; col++) {
        const int px = x + col;
        if (px < 0 || px >= PET_W) continue;
        const uint8_t bits = col < 5 ? g[col] : 0;
        for (int row = 0; row < 8; row++) {
            const int py = y + row;
            if (py < 0 || py >= PET_H) continue;
            canvas[py * PET_W + px] = (bits >> row) & 1 ? fg : BUDDY_BG;
        }
    }
}

// The species art is ASCII except for one acute accent (the duck's tail),
// written as UTF-8. Fold a multi-byte sequence into one cell: that accent
// becomes an apostrophe, anything else a '?'. Returns the bytes consumed.
static int next_char(const char* s, char* out) {
    const uint8_t b = (uint8_t)s[0];
    if (b < 0x80) { *out = (char)b; return 1; }
    int n = (b >= 0xF0) ? 4 : (b >= 0xE0) ? 3 : (b >= 0xC0) ? 2 : 1;
    for (int i = 1; i < n; i++) if (((uint8_t)s[i] & 0xC0) != 0x80) { n = i; break; }
    *out = (n == 2 && b == 0xC2 && (uint8_t)s[1] == 0xB4) ? '\'' : '?';
    return n;
}

static int cells(const char* s) {
    int n = 0;
    char c;
    while (*s) { s += next_char(s, &c); n++; }
    return n;
}

static void print_at_cursor(const char* s) {
    char c;
    while (*s) {
        s += next_char(s, &c);
        put_char(cur_x, cur_y, c, cur_fg);
        cur_x += BUDDY_CHAR_W;
    }
}

// ──────────────── helpers the species call (buddy_common.h) ────────────────
void buddyPrintLine(const char* line, int yPx, uint16_t color, int xOff) {
    cur_fg = color;
    cur_x = BUDDY_X_CENTER - cells(line) * BUDDY_CHAR_W / 2 + xOff;
    cur_y = yPx;
    print_at_cursor(line);
}

void buddyPrintSprite(const char* const* lines, uint8_t nLines, int yOffset, uint16_t color, int xOff) {
    for (uint8_t i = 0; i < nLines; i++)
        buddyPrintLine(lines[i], BUDDY_Y_BASE + yOffset + i * BUDDY_CHAR_H, color, xOff);
}

void buddySetCursor(int x, int y) { cur_x = x; cur_y = y; }
void buddySetColor(uint16_t fg)   { cur_fg = fg; }
void buddyPrint(const char* s)    { print_at_cursor(s); }

// ──────────────── species registry ────────────────
extern const Species CAPYBARA_SPECIES;
extern const Species DUCK_SPECIES;
extern const Species GOOSE_SPECIES;
extern const Species BLOB_SPECIES;
extern const Species CAT_SPECIES;
extern const Species DRAGON_SPECIES;
extern const Species OCTOPUS_SPECIES;
extern const Species OWL_SPECIES;
extern const Species PENGUIN_SPECIES;
extern const Species TURTLE_SPECIES;
extern const Species SNAIL_SPECIES;
extern const Species GHOST_SPECIES;
extern const Species AXOLOTL_SPECIES;
extern const Species CACTUS_SPECIES;
extern const Species ROBOT_SPECIES;
extern const Species RABBIT_SPECIES;
extern const Species MUSHROOM_SPECIES;
extern const Species CHONK_SPECIES;

static const Species* const SPECIES_TABLE[] = {
    &CAPYBARA_SPECIES, &DUCK_SPECIES, &GOOSE_SPECIES, &BLOB_SPECIES,
    &CAT_SPECIES, &DRAGON_SPECIES, &OCTOPUS_SPECIES, &OWL_SPECIES,
    &PENGUIN_SPECIES, &TURTLE_SPECIES, &SNAIL_SPECIES, &GHOST_SPECIES,
    &AXOLOTL_SPECIES, &CACTUS_SPECIES, &ROBOT_SPECIES, &RABBIT_SPECIES,
    &MUSHROOM_SPECIES, &CHONK_SPECIES,
};
static const uint8_t N_SPECIES = sizeof(SPECIES_TABLE) / sizeof(SPECIES_TABLE[0]);
static uint8_t species_idx = 0;

static const char* const NVS_NS = "pet";

void pet_init(void) {
    Preferences p;
    p.begin(NVS_NS, true);
    const uint8_t saved = p.getUChar("species", 0);
    p.end();
    if (saved < N_SPECIES) species_idx = saved;
}

uint8_t     pet_species_count(void) { return N_SPECIES; }
uint8_t     pet_species_idx(void)   { return species_idx; }
const char* pet_species_name(void)  { return SPECIES_TABLE[species_idx]->name; }

void pet_species_step(int dir) {
    species_idx = (uint8_t)((species_idx + N_SPECIES + (dir < 0 ? -1 : 1)) % N_SPECIES);
    Preferences p;
    p.begin(NVS_NS, false);
    p.putUChar("species", species_idx);
    p.end();
}

// ──────────────── clock and render ────────────────
static uint32_t tick_count = 0;
static uint32_t next_tick_at = 0;
static const uint32_t TICK_MS = 200;

void pet_clock(void) {
    const uint32_t now = millis();
    if ((int32_t)(now - next_tick_at) < 0) return;
    next_tick_at = now + TICK_MS;
    tick_count++;
}

uint32_t pet_ticks(void) { return tick_count; }

const uint16_t* pet_render(uint8_t state) {
    if (state >= PET_STATE_COUNT) state = PET_IDLE;
    memset(canvas, 0, sizeof(canvas));
    const Species* sp = SPECIES_TABLE[species_idx];
    if (sp->states[state]) sp->states[state](tick_count);
    return canvas;
}

#endif  // PET_ASCII
