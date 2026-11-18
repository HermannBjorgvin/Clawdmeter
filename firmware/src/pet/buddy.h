#pragma once
#include <stdint.h>

// The species interface from claude-desktop-buddy (MIT, see LICENSE-buddy),
// kept as the species files expect it. Each species lives in its own
// pet/buddies/<name>.cpp and exposes 7 state functions in PetState order:
// sleep, idle, busy, attention, celebrate, dizzy, heart.

// Per-species state function: takes the global tick count and draws the pet
// and any overlays for that state through the helpers in buddy_common.h.
typedef void (*StateFn)(uint32_t t);

struct Species {
  const char* name;
  uint16_t bodyColor;
  StateFn states[7];   // index by PetState (0=sleep .. 6=heart)
};
