#pragma once
#include <stdint.h>
#include <stdbool.h>

// The ASCII pets from claude-desktop-buddy, drawn into a small logical
// canvas (PET_W x PET_H, RGB565, black background) that the views scale up
// for the screen (pet_view.h). Built only with -DPET_ASCII.
//
// Canvas geometry, as the species files assume it: the 12x5-character body
// (72x40 px) is centred horizontally with its top at y 30, and the overlays
// (Zs, hearts, sparks) use the rows above it and the columns beside it.

enum PetState : uint8_t {
    PET_SLEEP, PET_IDLE, PET_BUSY, PET_ATTENTION, PET_CELEBRATE, PET_DIZZY, PET_HEART,
    PET_STATE_COUNT
};

#define PET_W        120
#define PET_H        82
#define PET_BODY_W   72     // 12 characters
#define PET_BODY_TOP 30
#define PET_FEET_Y   70     // first row below the body

void pet_init(void);                 // load the saved species (once, before any view)

// The animation clock: 5 frames a second, shared by every view so the seat
// and the big pet never disagree. pet_clock() advances it when a frame is
// due (call it every loop); a view redraws when pet_ticks() has moved on.
void     pet_clock(void);
uint32_t pet_ticks(void);

// Draw the current frame of `state` into the logical canvas and return it
// (PET_W * PET_H pixels, 0x0000 = background).
const uint16_t* pet_render(uint8_t state);

uint8_t     pet_species_count(void);
uint8_t     pet_species_idx(void);
const char* pet_species_name(void);
void        pet_species_step(int dir);   // +1 next, -1 previous; saved
