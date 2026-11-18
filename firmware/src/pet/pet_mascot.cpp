#ifdef PET_ASCII
// The pet in the meters' seat: mascot.h for an ASCII pet instead of Clawd
// (this file replaces mascot.cpp in a -DPET_ASCII build).
//
// The director drives it exactly as it drives the corner Clawd on a board
// without the full-size sprite: acts play in the seat, attention is shown
// from the seat, and there are no lurk trips. A pet has no walk cycle, so
// leaving and coming back are a short slide through the left edge; the
// director only needs them to take a moment and to end.
#include "../mascot.h"
#include "pet_buddy.h"
#include "pet_hooks.h"
#include "pet_view.h"
#include <Arduino.h>
#include <string.h>

enum Mode {
    M_SEATED,      // in the seat, doing whatever pet_persona() says
    M_ACT,         // a commanded reaction, for a set time
    M_LEAVING,     // sliding out through the left edge
    M_RETURNING,   // sliding back in to the seat
    M_AWAY,        // off screen
    M_ATTN,        // an alert is up: the attention animation, in the seat
    M_HELD,        // minimal mode's alert: one still attention frame
};

static PetView  view;
static bool     created = false;
static Mode     mode = M_SEATED;
static bool     minimal = false;
static bool     attn = false;
static int      seat_x = 0, seat_y = 0, off_x = 0;
static int      x = 0;                    // where the image is now
static int      slide_from = 0, slide_to = 0;
static uint32_t slide_start = 0, slide_ms = 1;
static uint8_t  act_state = PET_IDLE;
static uint32_t act_until = 0;
static uint8_t  drawn_state = 0xFF;
static uint32_t drawn_tick = 0;

#define SLIDE_MS 450        // seat to edge at 100 % speed

static uint8_t state_now(void) {
    switch (mode) {
    case M_ACT:  return act_state;
    case M_ATTN:
    case M_HELD: return PET_ATTENTION;
    default:     return pet_persona();
    }
}

static void draw(void) {
    drawn_state = state_now();
    drawn_tick = pet_ticks();
    pet_view_draw(&view, drawn_state);
}

static void place(int px) {
    x = px;
    lv_obj_set_pos(view.img, x, seat_y);
}

static void seat(void) {
    mode = M_SEATED;
    place(seat_x);
    pet_view_show(&view, true);
    draw();
}

static void slide(int to, uint16_t speed_pct, Mode m) {
    if (speed_pct == 0) speed_pct = 100;
    const int span = seat_x - off_x;
    const int dist = to > x ? to - x : x - to;
    slide_from = x;
    slide_to = to;
    slide_start = millis();
    slide_ms = span > 0 ? (uint32_t)SLIDE_MS * dist / span * 100u / speed_pct : 0;
    if (slide_ms < 1) slide_ms = 1;
    mode = m;
    pet_view_show(&view, true);
    draw();
}

static void seat_attention(void) {
    mode = M_ATTN;
    place(seat_x);
    pet_view_show(&view, true);
    draw();
}

// What a Clawd act means for a pet, and for how long (at 100 % speed, once).
static void act_for(const char* name, uint8_t* state, uint32_t* ms) {
    if (strcmp(name, "pointing") == 0)           { *state = PET_ATTENTION; *ms = 1600; }   // he noticed you
    else if (strcmp(name, "waving") == 0)        { *state = PET_HEART;     *ms = 2400; }   // a long turn is done
    else if (strcmp(name, "jumping") == 0)       { *state = PET_HEART;     *ms = 700;  }   // a tap: the startled hop
    else if (strcmp(name, "jumping happy") == 0) { *state = PET_CELEBRATE; *ms = 2000; }   // a fresh window
    else if (strcmp(name, "dancing") == 0)       { *state = PET_CELEBRATE; *ms = 2000; }   // a show, from the seat
    else                                         { *state = PET_IDLE;      *ms = 1000; }
}

// ─── API (mascot.h) ──────────────────────────────────────────────────────────
lv_obj_t* mascot_create(lv_obj_t* parent, int slot_x, int feet_y, int cell) {
    pet_init();
    pet_hooks_init();
    // The stock seat is drawn in `cell`-px cells (3 on a 480 panel, 2 on small
    // ones). The pet's art is thin 1-px strokes, so it takes 2x where Clawd
    // takes 3-px cells and stays 1x on small panels.
    const int scale = cell >= 3 ? 2 : 1;
    if (!pet_view_create(&view, parent, scale, true)) return NULL;
    // The body's feet on the seat's feet line, the body centred on the slot
    // Clawd's still pose would fill (72 px wide at 3-px cells, 48 at 2).
    const int slot_w = cell >= 3 ? 72 : 48;
    seat_x = slot_x + slot_w / 2 - pet_view_w(&view) / 2;
    // ...but never with the body past the left edge (the widest species fill
    // all 12 columns); the canvas's empty margin may hang off it.
    const int body_left = (PET_W - PET_BODY_W) / 2 * scale;
    if (seat_x + body_left < 2) seat_x = 2 - body_left;
    seat_y = feet_y - PET_FEET_Y * scale;
    off_x = -pet_view_w(&view);
    created = true;
    seat();
    return view.img;
}

bool mascot_seated(void)  { return created && mode == M_SEATED; }
bool mascot_away(void)    { return !created || mode == M_AWAY; }
bool mascot_busy(void)    { return created && mode != M_SEATED && mode != M_AWAY && mode != M_HELD; }
bool mascot_acting(void)  { return created && mode == M_ACT; }
bool mascot_free(void)    { return created && !minimal && mode == M_SEATED; }
bool mascot_out(void) {
    return created && !minimal && (mode == M_AWAY || mode == M_LEAVING || mode == M_RETURNING);
}
bool mascot_in_attention(void) { return attn || mode == M_ATTN || mode == M_HELD; }

// The pet animates on its own in every state, so there are no idle acts to
// switch on and off.
void mascot_autonomy(bool on) { (void)on; }

bool mascot_act(const char* anim, uint8_t times, bool snappy, uint16_t speed_pct) {
    (void)snappy;
    if (!created || minimal || mode != M_SEATED) return false;
    uint32_t ms;
    act_for(anim, &act_state, &ms);
    if (speed_pct == 0) speed_pct = 100;
    act_until = millis() + ms * (times ? times : 1) * 100u / speed_pct;
    mode = M_ACT;
    draw();
    return true;
}

void mascot_leave(uint16_t speed_pct) {
    if (!created || minimal) return;
    if (mode == M_AWAY || mode == M_LEAVING) return;
    slide(off_x, speed_pct, M_LEAVING);
}

void mascot_return(int from_side, uint16_t speed_pct) {
    (void)from_side;                       // the seat is by the left edge: always from there
    if (!created) return;
    if (mode == M_LEAVING) { slide(seat_x, speed_pct, M_RETURNING); return; }   // turn around
    if (mode != M_AWAY) return;
    place(off_x);
    slide(seat_x, speed_pct, M_RETURNING);
}

bool mascot_lurk(void) { return false; }

bool mascot_hurry_home(void) {
    if (!created || minimal) return false;
    switch (mode) {
    case M_SEATED:
    case M_ACT:
    case M_RETURNING:
        return true;
    case M_LEAVING:
        slide(seat_x, 250, M_RETURNING);
        return true;
    case M_AWAY:
        place(off_x);
        slide(seat_x, 250, M_RETURNING);
        return true;
    default:
        return false;                      // the alert has him
    }
}

void mascot_place_away(void) {
    if (!created) return;
    pet_view_show(&view, false);
    mode = M_AWAY;
    x = off_x;
}

void mascot_reset(void) {
    if (!created) return;
    attn = false;
    seat();
}

void mascot_set_minimal(bool on) {
    minimal = on;
    mascot_reset();
}

void mascot_attention(bool on, bool to_seat) {
    (void)to_seat;      // he's in his seat either way; the director sends him out if it wants that
    if (!created || on == attn) return;
    attn = on;
    if (minimal) {
        if (on) { mode = M_HELD; place(seat_x); pet_view_show(&view, true); draw(); }
        else    seat();
        return;
    }
    if (on) {
        // He may be out (the alert cut the screensaver short): he comes back
        // to his seat first, and mascot_tick starts the alert there.
        if (mode == M_SEATED || mode == M_ACT) seat_attention();
        else mascot_return(-1, 150);
    } else if (mode == M_ATTN) {
        seat();
    }
}

void mascot_repaint(void) {
    if (created && mode != M_AWAY) draw();
}

// ─── Tick ────────────────────────────────────────────────────────────────────
void mascot_tick(void) {
    if (!created) return;
    const uint32_t now = millis();
    pet_clock();
    const bool ticked = pet_ticks() != drawn_tick;
    if (mode == M_AWAY) return;
    if (mode == M_HELD) return;            // a still frame

    if (mode == M_LEAVING || mode == M_RETURNING) {
        const uint32_t t = now - slide_start;
        if (t >= slide_ms) {
            if (mode == M_RETURNING) { seat(); return; }
            mascot_place_away();
            return;
        }
        place(slide_from + (int)((int64_t)(slide_to - slide_from) * (int32_t)t / (int32_t)slide_ms));
    } else if (mode == M_ACT) {
        if ((int32_t)(now - act_until) >= 0) { seat(); return; }
    } else if (mode == M_SEATED) {
        if (attn && !minimal) { seat_attention(); return; }   // back in his seat for the alert
        if (!minimal && pet_take_level_up()) { mascot_act("jumping happy", 2); return; }
    }

    if (minimal) {                         // nothing moves in minimal mode
        if (state_now() != drawn_state) draw();
        return;
    }
    if (ticked || state_now() != drawn_state) draw();
}

#endif  // PET_ASCII
