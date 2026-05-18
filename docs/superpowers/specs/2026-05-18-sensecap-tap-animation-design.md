# SenseCAP tap-to-cycle-animation

**Date:** 2026-05-18  
**Branch:** sensecap-port

## Problem

On the SenseCAP Indicator D1L, swiping left/right navigates between screens (Splash → Usage → Bluetooth → Splash). There are 13 pixel-art animations on the Splash screen but only the usage-rate-appropriate one plays automatically. There is no way to manually cycle through them on SenseCAP (the Waveshare board uses a physical PMU button for this, which SenseCAP lacks).

Tapping on any screen currently calls `ui_cycle_screen()` (Usage ↔ Bluetooth), which duplicates swipe behavior and gives no access to the other animations.

## Goal

- Splash screen: tap → advance to the next animation (`splash_next()`)
- Usage / Bluetooth screens: tap → do nothing (screen navigation is swipe-only)
- Reset Bluetooth zone: unaffected (already uses `lv_event_stop_bubbling()`)

## Design

### `global_click_cb` (`ui.cpp`)

Replace the `TARGET_SENSECAP` branch body:

```c
// Before
if ((lv_tick_get() - sensecap_last_gesture_ms) < 400) return;
ui_cycle_screen();

// After
if ((lv_tick_get() - sensecap_last_gesture_ms) < 400) return;
if (current_screen == SCREEN_SPLASH) splash_next();
// non-splash: tap is a no-op; screen navigation is swipe-only
```

The 400 ms swipe-suppression guard is kept so a rightward swipe off the splash screen does not immediately advance the animation.

### Remove dead click listeners (`ui.cpp`)

| Location | Listener | Action |
|---|---|---|
| `init_usage_screen()` | `global_click_cb` on `usage_container` | Remove |
| `init_bluetooth_screen()` (inside `#ifdef TARGET_SENSECAP`) | `global_click_cb` on `ble_container` | Remove |

The splash container listener stays — it is what routes taps on splash to `global_click_cb`.

## Files changed

- `firmware/src/ui.cpp` — two removals + one substitution, all inside existing `#ifdef TARGET_SENSECAP` guards
