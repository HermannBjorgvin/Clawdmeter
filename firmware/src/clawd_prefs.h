#pragma once

// Clawd's settings from the app, kept in NVS so they apply from boot, before
// the app connects. The app sends both in every data payload.
//   liveliness       `lv`  1 No-nonsense .. 5 All-nonsense (clawd_policy.h)
//   attention anim   `aa`  seconds he points and waves for an alert; 0 = off

void clawd_prefs_init(void);          // after director_init(): load and apply the saved values
void liveliness_set(int level);       // apply; saved only when it changes
int  liveliness_get(void);
void attn_anim_set(int seconds);      // apply; saved only when it changes
int  attn_anim_get(void);
