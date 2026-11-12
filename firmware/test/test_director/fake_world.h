#pragma once
// The director test's stand-ins for everything around the director: the
// actors (corner and full-size sprite, performer, face) as
// small timed state machines with the real modules' contracts, and the
// situation the director reads (meters live, activity). Hooks come from
// the real signals module.
#include <stdint.h>

struct FakeWorld {
    bool meters_live;
    int  activity;
    bool asleep;
    bool has_big;          // PSRAM boards: the performer, lurk trips, the attention walk-on
    bool has_touch;        // BoardCaps.has_touch
};
extern FakeWorld world;

// Advance the actors that main.cpp ticks (performer, corner) to now.
void fake_actors_tick(void);

// Who is on screen right now. The one-Clawd rule: at most one at a time.
struct OnScreen {
    bool corner;           // the corner sprite (seated, acting or walking)
    bool big;              // the full-size sprite (lurk trip, attention)
    bool performer;        // the performer on the meters
    bool face;             // the face, any part of it
};
OnScreen fake_on_screen(void);
bool     fake_corner_seated_still(void);   // in his seat, not acting or walking
bool     fake_attention_animating(void);   // any part of the attention animation on screen
int      fake_clawds(const OnScreen& s);
