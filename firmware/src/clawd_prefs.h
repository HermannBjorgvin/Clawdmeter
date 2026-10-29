#pragma once

// Device settings from the app, kept in NVS so they apply from boot, before
// the app connects. The app sends them in every data payload.
//   liveliness       `lv`  1 No-nonsense .. 5 All-nonsense (clawd_policy.h)
//   attention anim   `aa`  seconds he points and waves for an alert; 0 = off
//   pace marker      `pm`  1 = pace marker on the usage bars (pace.h)
//   palette          `th`  the UI colours (theme_palette.h)
#include <stdint.h>
#include "theme_palette.h"

void theme_prefs_load(void);          // before ui_init(): the saved palette, so the UI is built in it
void clawd_prefs_init(void);          // after director_init(): load and apply the saved values
void liveliness_set(int level);       // apply; saved only when it changes
int  liveliness_get(void);
void attn_anim_set(int seconds);      // apply; saved only when it changes
int  attn_anim_get(void);
void pace_marker_set(bool on);      // apply; saved only when it changes
bool pace_marker_get(void);
void theme_set(const uint32_t p[TH_COUNT]);   // repaint live; saved only when it changes
