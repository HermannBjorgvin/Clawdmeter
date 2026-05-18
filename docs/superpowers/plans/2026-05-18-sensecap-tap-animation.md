# SenseCAP Tap-to-Cycle-Animation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** On the SenseCAP splash screen, tapping cycles to the next pixel-art animation; tapping on Usage or Bluetooth screens does nothing.

**Architecture:** Single change in `ui.cpp` — `global_click_cb` now dispatches on `current_screen`: splash → `splash_next()`, anything else → no-op. Two dead click listeners (usage_container, ble_container) are removed as cleanup. No new files, no new functions.

**Tech Stack:** C++, LVGL 9, Arduino/ESP-IDF, PlatformIO

---

## File Map

| File | Change |
|---|---|
| `firmware/src/ui.cpp` | 3 edits: `global_click_cb`, `init_usage_screen`, `init_bluetooth_screen` |

---

## Context you need

**Working directory for all commands:** `.worktrees/sensecap-port/`

**Build + flash:**
```bash
pio run -d firmware -t upload --upload-port /dev/ttyACM0
```

**Screenshot (verify visually):**
```bash
./screenshot.sh out.png /dev/ttyACM0
```
Read `out.png` with the Read tool to verify the frame.

**Serial monitor (watch logs):**
```bash
pio device monitor -d firmware -p /dev/ttyACM0 -b 115200
```
`splash: -> <name>` is printed by `splash_next()` each time the animation advances.

**No unit tests exist for this firmware** — correctness is verified by flashing and using the screenshot tool plus serial output.

---

### Task 1: Swap `global_click_cb` body for `TARGET_SENSECAP`

**Files:**
- Modify: `firmware/src/ui.cpp:476-487`

- [ ] **Step 1: Edit `global_click_cb`**

Find this block in `ui.cpp` (around line 476):

```cpp
static void global_click_cb(lv_event_t* e) {
    (void)e;
#ifdef TARGET_SENSECAP
    // Suppress the spurious LV_EVENT_CLICKED that fires at the end of a swipe gesture.
    extern uint32_t sensecap_last_gesture_ms;
    if ((lv_tick_get() - sensecap_last_gesture_ms) < 400) return;
    ui_cycle_screen();
#else
```

Replace the `TARGET_SENSECAP` branch body so the function reads:

```cpp
static void global_click_cb(lv_event_t* e) {
    (void)e;
#ifdef TARGET_SENSECAP
    // Suppress the spurious LV_EVENT_CLICKED that fires at the end of a swipe gesture.
    extern uint32_t sensecap_last_gesture_ms;
    if ((lv_tick_get() - sensecap_last_gesture_ms) < 400) return;
    if (current_screen == SCREEN_SPLASH) splash_next();
#else
```

- [ ] **Step 2: Build to verify no compile errors**

```bash
pio run -d firmware
```
Expected: `SUCCESS` with no errors or warnings about undefined symbols.

- [ ] **Step 3: Commit**

```bash
git add firmware/src/ui.cpp
git commit -m "On SenseCAP splash, tap cycles animation instead of changing screen"
```

---

### Task 2: Remove dead click listener from `usage_container`

**Files:**
- Modify: `firmware/src/ui.cpp` — `init_usage_screen()`

- [ ] **Step 1: Remove the listener**

Find this line inside `init_usage_screen()` (around line 260):

```cpp
    lv_obj_add_event_cb(usage_container, global_click_cb, LV_EVENT_CLICKED, NULL);
```

Delete it entirely.

- [ ] **Step 2: Build to verify no compile errors**

```bash
pio run -d firmware
```
Expected: `SUCCESS`.

- [ ] **Step 3: Commit**

```bash
git add firmware/src/ui.cpp
git commit -m "Remove dead usage_container click listener (tap is now a no-op on non-splash screens)"
```

---

### Task 3: Remove dead click listener from `ble_container`

**Files:**
- Modify: `firmware/src/ui.cpp` — `init_bluetooth_screen()`

- [ ] **Step 1: Remove the listener**

Find this block inside `init_bluetooth_screen()` (around line 292):

```cpp
#ifdef TARGET_SENSECAP
    lv_obj_add_event_cb(ble_container, global_click_cb, LV_EVENT_CLICKED, NULL);
#endif
```

Delete all three lines.

- [ ] **Step 2: Build to verify no compile errors**

```bash
pio run -d firmware
```
Expected: `SUCCESS`.

- [ ] **Step 3: Commit**

```bash
git add firmware/src/ui.cpp
git commit -m "Remove dead ble_container click listener (tap no-op on non-splash screens)"
```

---

### Task 4: Flash, test, and verify

**Files:** none — verification only

- [ ] **Step 1: Flash**

```bash
pio run -d firmware -t upload --upload-port /dev/ttyACM0
```
Expected: `SUCCESS`, device reboots.

- [ ] **Step 2: Navigate to splash screen**

Swipe left or right until you reach the Splash screen (pixel-art animation).

- [ ] **Step 3: Verify tap cycles animation**

Tap the screen. Check serial output:
```
splash: -> <animation name>
```
Each tap should print a new animation name. Tap several times and confirm a different animation plays each time.

- [ ] **Step 4: Screenshot a frame to confirm visually**

```bash
./screenshot.sh out.png /dev/ttyACM0
```
Read `out.png` with the Read tool and confirm the pixel-art character has changed from the previous animation.

- [ ] **Step 5: Verify swipe still navigates screens**

Swipe left from splash — should go to Usage screen.  
Swipe right from splash — should go to Bluetooth screen.  
Confirm no spurious animation advance happens at the end of the swipe (the 400 ms guard).

- [ ] **Step 6: Verify tap on Usage and Bluetooth does nothing**

Navigate to Usage screen. Tap. Confirm screen does not change.  
Navigate to Bluetooth screen. Tap outside the Reset zone. Confirm screen does not change.  
Tap the "Reset Bluetooth" zone — confirm it still triggers a BLE bond clear (serial log: `BLE bonds cleared` or similar).
