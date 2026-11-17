#pragma once

// Device settings from the app, kept in NVS so they apply from boot, before
// the app connects. The app sends them in every data payload.
//   liveliness       `lv`  1 No-nonsense .. 5 All-nonsense (clawd_policy.h)
//   attention anim   `aa`  seconds he points and waves for an alert; 0 = off
//   pace marker      `pm`  1 = pace marker on the usage bars (pace.h)
//   button actions   `k1` / `k2` / `k3`  what the primary, secondary and PWR
//                    buttons do (button_action.h)
//   palette          `th`  the UI colours (theme_palette.h)
//   Fable colour     `fc`  the Fable overlay in the weekly bar (fable.h)
//   rotation         `ro`  -1 = auto (the IMU), 0..3 = fixed quarter turns CW
//                    (boards with BoardCaps::has_rotation)
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
enum ButtonSlot { BTN_SLOT_PRIMARY, BTN_SLOT_SECONDARY, BTN_SLOT_PWR, BTN_SLOT_COUNT };
void    button_action_set(ButtonSlot slot, int32_t action);   // apply; saved only when it changes
int32_t button_action_get(ButtonSlot slot);
void theme_set(const uint32_t p[TH_COUNT]);   // repaint live; saved only when it changes
void fable_color_set(uint32_t rgb);           // repaint live; saved only when it changes

// Display rotation. Boards that rotate (BoardCaps::has_rotation) read
// display_rotation_quadrant() instead of imu_hal_rotation_quadrant(), in the
// flush path and the touch mapping alike, so picture and touch always agree.
#define ROTATION_AUTO (-1)
void    rotation_set(int8_t ro);             // ROTATION_AUTO or 0..3; saved only when it changes
int8_t  rotation_get(void);
uint8_t display_rotation_quadrant(void);     // 0..3: the IMU's on auto, else the fixed one
