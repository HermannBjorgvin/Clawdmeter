# Clawd's director: rooms, beats and the illusion of life

This is the firmware side of design system v0.4, sections 05–10: one Clawd who
lives on the meters, performs right there in front of them, and comes up to
the glass to check on you. The behaviour depends on the situation (hook events,
usage moving, a session reset, attention), not on gestures. A tap during a
performance or on the face means one thing: show me the meters. On the
meters themselves a tap is tapping the aquarium: the face comes up to see
what you want (see "A tap on the meters" below).

## Modules

| File | Role |
|---|---|
| `firmware/src/clawd_director.{h,cpp}` | Owns the rooms and runs the beats. It is the only module that decides what Clawd does next. |
| `firmware/src/clawd_policy.{h,cpp}` | Pure decision rules, unit-tested in `test/test_clawd_policy`: show gate and show group, screensaver gate, where attention returns to, how long a reaction waits for him, the corner seat's headroom, turn tracker, no-repeat picker, jitter, the resting face's rhythm, what a tap on the meters earns (the peek, how he sets off), where the face looks and how long it stays curious, and all tunables (`ClawdTuning`). |
| `firmware/src/mascot.{h,cpp}` | Clawd on the meters: the corner Clawd in his seat, plus the full-size sprite used for the lurk trip and the attention walk-on. Does in-room secondary action (idle acts, lurk trips) when the director allows it. |
| `firmware/src/performer.{h,cpp}` | Clawd performing on the meters: the official scenes at full size on a 60×60-cell grid, in front of the meters. He stands, walks on and off through either edge, or performs a scene. PSRAM only. |
| `firmware/src/face.{h,cpp}` | The #198 face: a field on `lv_layer_top()` with the eyes as small LVGL rectangles on it. It is the screensaver and the held peek. |
| `firmware/src/face_motion.{h,cpp}` | The face's rise and drop as a pure function of time (vertical only, one size: overshoot and settle, anticipation, follow-through; see "The face's motion"), unit-tested in `test/test_face_motion`. |
| `firmware/src/field.{h,cpp}` | A flat-colour shape on an overlay layer (a rectangle plus two small extra rectangles) that redraws only the strips that change colour (see "Redraw cost and frame rate"). The face is a field. |
| `firmware/src/perf.{h,cpp}` | The `perf` serial command: refresh count, timing and pixels sent for the last face rise, its settle, face drop and performance. |
| `firmware/src/clawd_anim.{h,cpp}` | Read-only view of the official animations. It is the only translation unit that includes `splash_animations.h` (about 400 KB of static data). |
| `firmware/src/ui.{h,cpp}` | Draws the meters only: cards, clock, status line, battery, the seat. Stale data dims the meters (see below); it never adds a second creature. |

`main.cpp` feeds the director per payload (`director_on_payload` after
`signals_ingest`, then `director_alert` / `director_alert_clear`), on buttons
(`director_button`), and once per loop (`director_tick`). A prompt or back-after-gap event (`e:"p"`
/ `e:"b"`) also calls `idle_note_activity()` first, so a dimmed or dark panel
fades back in before he notices you; an alert already did. A Stop or tool
activity does not wake the panel: Claude working alone isn't you. Taps come straight
from LVGL: an indev `LV_EVENT_PRESSED` callback, so the reaction starts on the
next frame, not on release.

## Rooms

- **Meters**: home, the default, and where every path ends. The corner Clawd
  sits in his seat (3 px cells on a 480 panel). He also performs here, full
  size (8 px cells), in front of the panels (below).
- **Face**: an overlay on `lv_layer_top()`. It rises from behind the bottom
  bezel, overshoots and settles (at rest after ~595 ms, still after 860 ms),
  and drops after a little anticipation hop (510 ms). See "The face's motion".

**Performing on the meters.** The performer is the topmost object on the
meters screen and fully opaque, like the attention walk-on: he's never
dimmed or see-through, and while he performs he covers part of the panels
(mostly the weekly one). That's a deliberate trade for the illusion of life:
a show lasts about 20 s and a tap sends him off at once. His feet stand on
the bottom edge of the last panel (`performer_init`'s
`feet_y`), clear of the status line, which is Clawd-orange too. The
performer's image is hidden whenever he's off screen, so the meters' own
redraws never blend a screen-sized transparent image, and each frame
invalidates only the cells that changed.

![A show on the meters (simulator)](../screenshots/director/06-show-scene.png)

Continuity rules, as the code enforces them:

- There is only ever one Clawd. The corner sprite, the full-size sprite, the
  performer and the face are never on screen together; each beat waits for
  one to be off screen (`mascot_away()`, `performer_visible()`, the face's
  drop) before the next appears. `test/test_director` runs the real director
  against timed stand-ins for these actors and checks this on every tick
  through a seeded fuzz of everything that can happen.
- He enters and leaves through an edge. Walks use the official walking gait
  with foot-locked translation (`clawd_gait_cells`) and mirror when heading
  left. When the corner leaves, he takes the nearest edge; the performer
  comes on and goes off through the left edge, where the corner Clawd left
  and comes back.
- The screen never cuts. The face rises over the meters only once he's gone,
  and drops back onto them before he walks on.
- The normal status line rotates Claude Code's whimsical thinking verbs as
  the all-is-well indicator. This is the owner's chosen behavior, not an
  activity report. Keep the spinner and ellipsis. Disconnected is "Waiting";
  minimal mode uses fixed status text instead of rotating verbs.
- Stale data keeps the one-Clawd rule. When the link is up but no fresh usage
  has landed for `DATA_FRESH_MS` (90 s), or the daemon sent `{"ok":false}`,
  the meters stay where they are at 40 % opacity (LVGL `opa`, which scales the
  draws and needs no layer buffer, so it's free on the C6) and the status line
  alternates "Stale…" (or "No data…" before the first payload) with
  "Listening…". The corner Clawd stays in his seat. Shows and the screensaver
  need live meters, so they wait.

## Beats

The director runs one beat at a time. A beat is a list of steps, and each step
issues commands. The next step starts once every actor has settled (face
not moving, performer and mascot not busy) and any explicit pause has
passed. Beat changes always print; every step prints too after
`director_set_trace(true)` (the sim turns it on with `SIM_TRACE=1`).

| Beat | Trigger | Steps |
|---|---|---|
| `home` | default | Corner autonomy on: jittered idle gaps (±40 % of 10 / 7 / 5 / 3.5 s by mood), mood-picked acts, never the same act twice running, and lurk trips (PSRAM). Also checks the gates below. |
| `corner` | notice, wave, joy, or after a transition | Waits for the commanded corner act or walk to finish, then `home`. |
| `show` | `a == 3` held for 5 min; at most one per 15 min; hooks seen; meters live | Points at the bar (or, if he's out on a lurk trip, heads straight out), walks off left at 1.5×, walks back on full size at 1.5×, one scene (loop held 16 s, so about 20 s), waves, walks off left, and the corner Clawd walks back in to his seat. Without the performer (no PSRAM): points, then dances twice (jumps for joy twice when usage is Heavy) and waves, all in his seat. |
| `screensaver` | 10 min with no engagement; needs hooks seen or a window that hasn't started; meters live | Walks off through the nearest edge (a lurk trip finishes its peek first), face rises and rests (blinks every 2–6 s, about one in five a double, glances where the far eye travels 1.5×). Every 3–5 min, a calm dip: the face drops onto the meters, he walks on, plays one calm scene (magnifier or walking, alternating) and walks off, then the face rises and he rests again. Without the performer the corner Clawd walks back to his seat, sits for the scene's length and walks off again. |
| `face-home` | tap, prompt, button or attention while the screensaver face is up; a tap, attention or the end of its curiosity while the peek face is up | If woken by a prompt or tapped during a peek, he blinks first (noticing), then the face drops into the meters, and the corner hurries home at 1.5×. |
| `run-home` | tap or attention during a performance; a tap, prompt, button or usage moving during a calm dip | The scene releases to the idle pose on the spot, holds 200 ms, and he runs off left, ramping from 1.5× to 3× over 400 ms. Once he's gone, the corner walks home at 1.5×. |
| `peek` | any tap on the meters (`home` or `corner`), live or stale (below) | Seated: a startled hop (`jumping` from its crouch, 2×), then he ducks out through the nearest edge at 4×; already moving or out: straight out at 4×. The face rises, eyes glance toward the tap's x and hold 1.6–2.4 s, a blink (30 % double), the look comes back to you, and the face stays up, curious, for 5 min: resting like the screensaver face (blinks every 2–6 s, some double), its glances going back toward the tap most of the time. Then it goes back on its own; a tap sends it back right away (with the acknowledging blink). Both go through `face-home`: the drop, and he walks back to his seat at 1.5×. (below: what else happens while it's up) |
| `attention` | `m` (alert) in a payload | The corner walks off at 2×, the big Clawd (12 px cells) walks on at 2×, then waves and points alternately (no PSRAM: he waves and points in his seat). The alert lasts until a tap, `x` (clear) or 2 min; his animation lasts the **attention animation** time (below), 2 min by default, after which the big one walks off left and the corner Clawd sits in his seat while the alert stays up. Off: no animation, he sits in his seat. A new alert animates again. Interrupts a show or the screensaver through `face-home` / `run-home`, and then returns to where it left off (below). |

**Which scene a show plays.** The trigger (activity 3) lifts the live mood
group to Heavy, so the show reads the usage-only base group instead, floored
at Active (`clawd_show_group`): idle-to-active usage plays the Active set
(laptop, dancing, skateboard, soccer), real burn plays the Heavy set (racing
car, cloud, sailing, jumping happy). Shows never repeat the previous show's
scene, and calm dips never repeat the previous calm scene (tracked separately,
so a calm dip doesn't reset the show's no-repeat memory).

**Where attention returns to** (`attention_resumes_screensaver`):

- It interrupted the **screensaver** and nobody showed up (the alert timed out,
  or was cleared with no tap, button, prompt or tool activity since it
  started): the big Clawd walks off, the corner Clawd stays out of sight, and
  the face rises again. The screensaver carries on as if never interrupted.
  A refreshed alert (a second `m` while one is up) keeps any sign of you seen
  so far, and a new alert that lands while the last one is still leaving for
  the screensaver inherits that origin.
- It interrupted a **show**: the show ends gracefully home. It was a 20 s
  treat; replaying it after an alert of up to 2 minutes would be stale, and the
  show gate already counted it, so the next one is earned as usual.
- A **tap** dismissing it, or any sign of you during it, always ends on the
  meters ("show me the meters"), with the corner Clawd walking home.

Small reactions on the meters, from `home` or `corner`:

- **Noticing**: `e:"p"` or `e:"b"`. He points once (anticipation). From the
  screensaver the same event wakes him (`face-home` with the blink).
- **Long-turn wave**: `e:"s"` when the turn (first prompt to stop) ran at
  least 2 min. He waves once.
- **If he's out** (a lurk trip, or walking back from one): the reaction isn't
  dropped. He cuts the trip short (the rest of the peek plays at 3×), hurries
  back to his seat at 2.5× (or turns around if he was still heading out), and
  then reacts. The reaction waits up to 15 s (`react_pending_ms`) for him. A
  reaction arriving while a commanded act plays queues behind it.
- **Fresh window**: `session_reset` from signals (real samples only, so never
  on a replay). He jumps happy twice, in the same loop iteration as the chime.
  If he's out on a trip he hurries back for it; if he's in another room it
  waits up to 5 min for him to sit down. It outranks the wave.

### A tap on the meters (the aquarium)

While the director is on the meters (`home`, or `corner`: a commanded act, a
walk home), every tap draws his attention and the face comes up to see what
you want (`clawd_tap_react`). There's no cooldown and no lesser reaction:
the face is what a tap on the glass gets.

- **Seated** (or in an idle act of his own): a startled hop on the next frame
  (`jumping` from frame 1, its crouch, not the idle frame every act opens
  with, held 330–580 ms), at 2×, then he ducks out through the nearest edge
  at 4×. His seat is a few steps from the left edge, so the face starts
  rising about 0.75 s after the tap. One Clawd: he leaves before the face
  rises (the "enter and leave through an edge" rule). Letting the rising face
  cover his seat instead would put the face and the corner sprite on screen
  together for most of the rise, since the seat is at the top.
- **Already up and about** (a commanded act, walking home, a lurk trip): he
  heads straight out at 4×. A lurk trip finishes its peek at 4× and he stays
  out; a walk out for a lurk becomes a plain exit. A queued corner reaction is
  dropped.
- **Stale or pairing meters** don't stop it: the face covers the hint for as
  long as it's up, and the hint is back when it drops.
- **A tap during the hop or the exit** is absorbed (nothing queues).
- **A tap while the peek's face is up** (or still rising): he blinks and the
  face drops (`face-home` with the noticing blink), then he walks back to his
  seat. Once the face is already dropping, the tap is absorbed. A tap while
  he's walking back raises the face again.

**While the peek's face is up** it's curious for `peek_curious_ms` (5 min,
counted from when it starts to rise; `peek_hold`):

- It rests like the screensaver face, but its glances go back toward where
  you tapped 60 % of the time (`face_glance_px`; the others are the usual
  left / right glances). Blinks are irregular, some double.
- After 5 min it goes back on its own: `face-home` without the blink, the
  drop, and he walks back to his seat.
- Prompts, stops, usage moving, buttons and PWR don't drop it, and the usual
  corner reactions (pointing, the long-turn wave) are skipped: he's up at the
  glass, not in his seat. They still count as engagement. A fresh-window joy
  keeps its usual 5 min wait for him to sit down; a show that comes due waits
  for him to be home.
- **The meters going stale or the link dropping** doesn't drop it either
  (you asked to see him). The stale hint shows once it's gone.
- **The screensaver can't start under it.** The tap that raised it is
  engagement, and the curiosity (5 min) runs out well before the screensaver's
  10 quiet minutes, so the face always goes back first; the screensaver can
  come 5 min later, from the meters, as usual.
- **Attention** interrupts it like anything else (`face-home` without the
  blink, then the big Clawd). When the alert ends it's the meters, not the
  face: the peek started from the meters (`ATTN_FROM_HOME`), and putting a
  face back up after you've been fetched would be a non sequitur.
- The 30-minute idle fade still applies (in practice the face is long gone
  by then), and a waking tap only wakes the panel.

Unchanged: a tap during attention dismisses it, and a tap during a
performance or on the screensaver face still means "show me the meters". In minimal mode the tap
does nothing (he stays on the still frame). On the C6 it all works: the face
is LVGL rectangles and the corner sprite has its internal-RAM buffer. A tap
that wakes a dark panel is swallowed by `idle_consume_wake_press` (it never
reaches LVGL), so the waking tap only wakes the panel. Every tap counts as
presence: the press edge resets the idle timer, and `director_tap` counts it
as engagement for the screensaver clock.

**The seat leaves headroom for the jump.** Jumping happy rises 12 cells above
the idle pose (30 cells tall). `ui.cpp` lowers the seat's feet line just enough
for it to clear the top edge by 2 px without touching the first panel
(`corner_seat_feet`): on 480×480 the feet sit at y 92, and the 368×448
compact layout starts its panels at y 96 to make room. A 240 px panel has no
such room (the seat would land in the first panel), so there `mascot_act`
swaps any act taller than the headroom for dancing (17 cells) rather than clip
him.

**Engagement** resets the screensaver clock. Engagement is any fired hook
event, any activity level above 0 (tool calls are hook events too), a tap, a
button, an alert, or a real sample where session % went up. Usage going up
also wakes the screensaver, which covers the slow-clock-only case.

## Animation principles and implementation

| Principle | Implementation |
|---|---|
| Anticipation | Pointing before a show and on a prompt, a blink before the face drops on a wake, the face's crouch-and-hop before it drops, a beat before the run-off, a turn toward the edge (walking intro frames) before every exit. |
| Follow-through, overlapping action | The face's eyes lag its body, vertically, so they reach rest a beat later. |
| Timing | Taps act on `LV_EVENT_PRESSED`. `performer_release()` snaps to the idle pose in the same frame. |
| Secondary action | `clawd_jitter` for idle gaps, `clawd_pick_no_repeat` for acts and scenes, `face_plan_next` for irregular and double blinks and glances. |
| Staging | The meters hold still while he performs behind them (only the clock and the status line tick), and there's one actor at a time. |
| Slow in, slow out | Ease-out into the face's overshoot, ease-in for its fall, the run-off's speed ramp. Walks begin on intro frames and end on the outro. |
| Exaggeration | Wave for a long turn, show for a busy stretch, jumping happy for a reset, the big Clawd for attention. |
| Appeal | The face appears for the screensaver and when you tap the meters: you drew his attention, so he comes up to look. |

## The face's motion

The face is Clawd's body seen up close, so when it comes up to the glass and
goes away again it should move like a soft, curious creature, not a sliding
panel. The motion is hand-keyed in the classic way (Thomas & Johnston's
principles) and computed as a pure function of elapsed time
(`face_motion.{h,cpp}`, unit-tested in `test/test_face_motion`): the same
input always gives the same frames, so durations stay fixed and the motion
never depends on frame rate. It has no randomness.

**Vertical only.** The top edge is always flat and the eyes level: there's no
tilt, lean or sideways drift. A real tilt would mean `transform_rotation` on
a full-screen object, rendered through a full-screen ARGB layer every frame
(about 0.9 MB on 480×480): it kills the frame rate and can't be allocated on
the no-PSRAM C6 boards. Faking it as a stepped shear of the top edge aliases
into a visible staircase on the panel.

**One size.** The face never changes size: the body is always full bleed and
the eyes keep their shape. Narrowing the body (arms out) or squashing the eyes
read on the panel as the face resizing, so the character lives in its
position alone:

- **Follow-through** is vertical lag: the eyes trail the body by 40 ms
  (clamped), so they keep going when the body stops and catch up after.

**The rise** (peek, screensaver), 860 ms in all:

1. 0–500 ms, *up with slow-out*: ease-out from below the bezel to 44 px (on
   480) past the rest position.
2. 500–860 ms, *settle*: an underdamped spring (period 300 ms, decay 110 ms)
   brings him back down through rest at ~595 ms into a small dip (about
   12 px of black at the top edge) and back. The eyes reach rest a beat
   after the body.

`face_moving()` stays true through the settle, so nothing behind the face
changes while the dip shows a sliver of the meters. The director's next step
(a blink, a glance) starts once he's still.

**The drop** (wake, calm dip, peek over), 510 ms, full width
the whole way:

1. 0–150 ms, *anticipation*: a quick crouch (6 px down), then a little hop
   up (28 px).
2. 150–510 ms, *falls with slow-in*: ease-in straight down past the bezel.
   The eyes lag a little, riding up the body as it accelerates away.

**Budget.** The body is a flat rectangle, so each frame invalidates its exact
old XOR new: the rows its top edge moved through, plus the side strips while
it widens on the rise, plus the arms' old XOR new. An open eye's LVGL box is
exactly the eye (it widens to hold the squint chevron only while blinking),
a solid rectangle, so it too redraws only its old XOR new: the few rows it
moved and the columns it stretched, not the whole eye twice. The face costs
about half the pixels per frame of the plain slide (table in "Redraw cost
and frame rate"). `perf` reports the rise up to the moment he first reaches rest
(`face-rise`), the settle after it (`face-settle`), and the drop
(`face-drop`).

**The performer's run-off** (`run-home`) gets follow-through through
timing only, never new frames: the scene releases to the idle pose and holds
a beat (anticipation), then the walk-off ramps from 1.5× to 3× over its first
400 ms (ease-in) instead of starting at full speed.

## Round panels

With `BoardCaps.round_panel` the glass is the circle inscribed in the
panel, so Clawd walks on and off along it. `panel_row_span()` in
`splash_geometry.h` gives the visible span of a row (the whole width on a
rectangular panel, the circle's chord on a round one) and every entrance
uses it at the row he walks on: the corner Clawd leaving and coming back
(`mascot.cpp`: `off_left_x` / `off_right_x`), the full-size attention walk-on
and walk-off, the lurk peek, and the performer's walks (`performer.cpp`: the
grid cells on the glass at his floor row). None of them spends time walking
where there is no glass, and nobody starts at the bounding box's edge.
Rectangular panels are unchanged. Unit-tested in `test/test_splash_geometry`.

## Alert dismissal and expiry

A tap or the device's 2-minute attention timeout ends the local alert and
sends `{"dismiss":true}` on BLE TX (`…-0003`). The app clears the alert it
sent, including its tray state, without echoing a clear back. A host `x`
clear ends the device alert without a dismissal notification.

On a board without a touchscreen (`BoardCaps.has_touch` false) the main
button is the dismiss gesture: BOOT, or the PWR key when it is the board's
only button (`button_count` 0). `director_button(true)` ends an active alert
the same way a tap does and reports the press as consumed, so `main.cpp`
doesn't send push to talk (or cycle brightness) for it. With no alert up
the button keeps its normal role. A board with neither touch nor a button
lets the alert time out. `SIM_NO_TOUCH=1` plays such a board in the
simulator.

The app also expires uncleared alerts after 2 minutes; Hooks test alerts
expire after 30 seconds. If the alert was sent, app expiry queues a clear
for the device. These app limits are separate from the device's 2-minute
attention timer (`SIM_FAST` compresses that device timer to 20 seconds).

## Minimal mode

When `BoardCaps.minimal_ui` is true, `minimal_mode()` returns true. This is
a board capability, with no NVS override, desktop setting, or serial toggle.
`SIM_MINIMAL=1` sets the simulator capability at startup. In this mode:

- No performances, face, lurk trip, idle acts, noticing, wave or joy.
- The corner Clawd stays on the official still frame (walking frame 0).
- Attention: a held pointing frame (pointing frame 16) in the seat, and the
  message in the status line, truncated rather than scrolled. A tap dismisses
  it, as do `x` and the 2 min timeout.
- The status line shows a fixed glyph and one word ("Connected", "No data",
  "Waiting") and redraws only when the state changes.

## Boards without PSRAM (C6)

- Corner Clawd: animated. Its buffer, sized for the largest corner act
  (jumping happy, 28×30 cells × 3 px, about 23 KB), falls back to internal RAM.
- Face: plain LVGL objects, the same as on S3.
- No performer (its buffer is about 690 KB): a show is corner acts (pointing,
  dancing or jumping happy twice, a wave) and a calm dip is the corner Clawd
  walking back to his seat for a while (see Beats).
- No full-size sprite (about 250 KB), so there's no lurk trip, and attention
  is a wave and point in his seat (he walks back to it first if he's out),
  for the attention animation's time.

## Buttons

- BOOT (Space) and SECONDARY (Shift+Tab): unchanged. They also count as
  engagement and wake the screensaver.
- PWR short press: cycles brightness and counts as engagement (waking the
  screensaver, face or calm dip). Hold to pair is unchanged.
- Shows are earned rather than opened by a button or a tap.

## Tunables

`ClawdTuning` in `clawd_policy.h` holds all the timings (the numbers here
are Some-nonsense, liveliness 3; see "Liveliness" below): show after 5 min,
cooldown 15 min, show scene loop 16 s, quiet 10 min, calm dip every 3–5 min,
calm scene loop 10 s, alert timeout 2 min, long turn 2 min, joy wait 5 min,
reaction wait 15 s, blinks every 2–6 s, peek look 1.6–2.4 s, peek
curiosity 5 min, double blink on a peek 30 %, curious glances back toward the
tap 60 %. `clawd_tuning_fast()` compresses them for the sim (`SIM_FAST=1`;
the peek's curiosity becomes 12 s, under the 15 s quiet time as in the
defaults).

## Liveliness

The app's **Animation silliness** slider (config `liveliness`, payload `lv`, 1–5,
default 3) sets how lively Clawd is. The device stores the last value in
NVS (`clawd_prefs.cpp`, key `lively`) so it applies from boot. One pure
function, `clawd_tuning_scaled()`, turns the base tuning (the defaults, or
`SIM_FAST`'s) into the effective one; the director, the corner and the
status line read nothing else, and `director_set_liveliness()` applies a
change at once (a show or screensaver the new level rules out goes home).

| Level | What changes |
|---|---|
| 1 No-nonsense | No shows, screensaver, idle acts or lurk trips, and no reactions (noticing, long-turn wave, fresh-window joy). The status line's verb changes once a minute and the spinner holds still, so with no taps or alerts only the clock and that word redraw. A tap still gets the peek, an alert still gets the attention walk-on. |
| 2 | Rate-like timings ÷ 0.6: show after ~8.3 min, cooldown 25 min, calm dips every 5–8.3 min, corner still time ×1.67, wave after a 3.3-min turn. |
| 3 Some-nonsense | The base timings above. |
| 4 | ÷ 1.8: show after ~2.8 min, cooldown ~8.3 min, calm dips every ~1.7–2.8 min, still time ×0.56, wave after ~67 s. |
| 5 All-nonsense | ÷ 3: show after 100 s of activity **2** or more (not just 3), cooldown 5 min, calm dips every 1–1.7 min, still time ×0.33 (never under 1.5 s), wave after a 40-s turn. |

The quiet time before the screensaver (10 min) doesn't scale: it measures
you being away, not him. Levels 2 and 4 sit between their neighbours on a
log scale (0.6 / 1 / 1.8 / 3). Native tests: `test_clawd_policy` (the
scaling), `test_director` (No-nonsense sits still through activity, prompts,
stops, usage and a reset but still peeks on a tap; turning it down mid-show
ends the show; the one-Clawd fuzz at every level). In the sim:
`SIM_CMDS="300:liveliness 1"`, or `"lv":1` in a scenario line.

## Attention animation

The app's **Attention animation** (config `attention_anim` = `off`, `10s`,
`30s` or `2m`; payload `aa` in seconds, 0 / 10 / 30 / 120) sets how long
Clawd points and waves for an alert. It's stored in NVS (`clawd_prefs.cpp`,
key `attn_anim`) and liveliness doesn't touch it. Only his animation stops:
the message, the dismissing tap, `x`, the 2-minute alert timeout and the
`{"dismiss":true}` notify are unchanged. When the time is up the big Clawd
walks off and the corner Clawd walks home to his seat (no PSRAM: the seat's
wave stops); he sits still there until the alert ends. `clawd_attn_anim_ms()`
maps the setting onto the tuning's alert timeout (2 min = the whole alert),
so under `SIM_FAST` 30 s is 5 s of the 20 s alert. Native tests: Off and a
10 s setting, with and without the full-size sprite, and the one-Clawd fuzz
at Off / 10 s / 30 s (`test_director`). In the sim:
`SIM_CMDS="300:attention_anim 0"`, or `"aa":0` in a scenario line.

## QA

- Native: `pio test -d firmware -e native -f test_clawd_policy` (the rules)
  and `-f test_director` (the director against timed stand-ins for its
  actors: one Clawd on screen at every tick of a seeded fuzz, every beat
  reached, and nothing left hanging once the input stops).
- Sim: `SIM_FAST=1 SIM_SCENARIO=sim/director.jsonl`, plus `SIM_SHOTS` /
  `SIM_TAPS` for headless capture (see `SIM-USAGE.md`) and `SIM_TRACE=1` for
  the per-step trace. The interruptions (lurk cut short, attention resuming
  the screensaver, PWR during a calm dip, wake) have their own scenario, driven by
  `SIM_CMDS` (scripted serial commands; `pwr`, `prompt`, `stop` and `alert`
  stand for the PWR button, the `e` / `t` keys and the `m` key):

  ```bash
  SIM_FAST=1 SIM_SCENARIO=sim/director-resume.jsonl \
  SIM_CMDS="2500:clawd lurk;5200:prompt;34000:alert;67500:pwr;80000:prompt" \
  .pio/build/sim/program
  ```

  A reference set of captures is in `screenshots/director/`, all simulator
  output (01–25 the main walkthrough, 26 stale data, 27–29 a lurk cut short,
  30–31 attention handing back to the screensaver, 32 PWR during a calm dip,
  33–34 the jump at 368×448 and its 240×240 fallback, 37–38 a show at
  368×448 and 240×240, 39 No-nonsense mid-walkthrough (he sits still), 40–42 the attention
animation at 30 s (waving, then back in his seat with the alert still up)
and Off, 35-peek-* a tap on the meters: crouch, hop, ducks
  out, face rises, looks toward the tap, blinks, still up seconds later (a
  glance back toward the tap, a blink), the tap on the face blinks and drops
  it, back in his seat; then a tap on stale meters (12–13) and while pairing
  (14), whose face goes back on its own when its curiosity runs out (15–16);
  36-face-motion-sheet.png, a 33 ms contact sheet of a peek's rise, settle
  and drop, 36-face-motion.gif the same at real timing, and 36-* key frames).

  The tap captures come from a single live payload (any one-line scenario
  with a long `hold_ms`) with scripted taps:

  ```bash
  SIM_FAST=1 SIM_TRACE=1 SIM_SCENARIO=<one live payload> \
  SIM_TAPS="3000@420,300;16000@240,300;19000@60,300" \
  .pio/build/sim/program
  ```

  3 s: the peek, looking right; the face rises about 1.15 s after the tap and
  stays up, resting and glancing back toward the right, until the tap at 16 s
  blinks and drops it. 19 s: he's back in his seat and the next tap brings it
  straight back up (no cooldown), this time curious about the left; left
  alone, it goes back on its own after `SIM_FAST`'s 12 s curiosity
  (`peek over (curiosity)` in the log). Add `SIM_CMDS="2000:link"` to drop the
  BLE link first: the tap on the pairing hint gets the face just the same.
  For the stale (dimmed) meters, let one payload go 90 s without a refresh
  and tap after that.
- Hardware: send `clawd show`, `clawd screensaver`, `clawd attention`,
  `clawd joy`, `clawd lurk`, `clawd peek [x]` (the peek now, as a tap at x)
  or `clawd home` on the serial console, then `screenshot`
  (`./screenshot.sh`). This replaces swapping the boot screen in `main.cpp`.
  The device boots to the meters. `perf` prints the redraw cost of the last
  face rise, face drop and performance (below).

## Redraw cost and frame rate

The face is a big flat rectangle that moves. As an ordinary LVGL
objects every move invalidates their whole old and new area, so each frame of
the rise was a near full-screen render plus a full panel flush (about 460 KB
over QSPI on the 2.16), and LVGL's default 33 ms refresh capped it at 30 fps.
On hardware that read as choppy.

- **Fields** (`field.{h,cpp}`): the face's orange is a full-screen object
  that never moves and draws its rectangle in `LV_EVENT_DRAW_MAIN`. Moving
  the rectangle invalidates only old XOR new: the rows the rising face just
  covered. The face's eyes are small children that move normally. At rest a
  field is `LV_OBJ_FLAG_HIDDEN` with an empty rectangle. Showing, hiding or
  restacking a full-screen object would invalidate the whole screen even
  when it draws nothing, so those run with
  `lv_display_enable_invalidation(false)` around them (`field_quietly`).
- **The performer** (PSRAM boards) repaints only the cells that changed and
  invalidates their bounding box, instead of the whole screen-sized image
  every frame, and is hidden (quietly, the same way) whenever he's off
  screen. A show in the sim averages about 18 k px/frame (7.7 % of the
  panel).
- **Refresh period 16 ms** (`-DLV_DEF_REFR_PERIOD=16` in every env, which also
  sets the animation and touch-read timers). It's a cap, not a cost: LVGL
  only renders what was invalidated, and a frame that takes longer than the
  period just delays the next one, so a slow bus gets fewer frames, not more
  work per frame. With fields a rise frame is a few strips plus the eyes,
  about 10 % of the panel: roughly 45 KB per frame on the 480×480 boards,
  which should fit well inside one 16 ms frame on the 2.16's 40 MHz QSPI and the C6 boards'
  QSPI, the LCD-1.54's SPI (240×240, a quarter of the pixels), and the
  LCD-4's RGB framebuffer copy. If one board turns out not to keep up, give
  its env its own `-DLV_DEF_REFR_PERIOD`.

- **The face's eyes** are solid rectangles, so `apply_eyes` moves them with
  invalidation paused (`field_quietly`) and invalidates only old XOR new
  itself (`field_invalidate_change`, at most four areas per eye, so the
  body, arms and both eyes stay well inside LVGL's 32-entry buffer). A
  squinting eye's box isn't solid (the chevron) and moves the ordinary way.

Measured in the sim (`SIM_FAST=1`, a peek and a tap; pixels per frame is the
bus-independent part; the first two columns are earlier implementations,
kept for comparison):

| | plain LVGL objects | fields, plain slide | fields, face motion |
|---|---|---|---|
| face rise (to rest) | 18 frames, 154 k px/frame (67 % of the panel) | 600 ms: 35 frames, 22 k px/frame (9.5 %) | ~600 ms: 35 frames, 11–11.5 k px/frame (4.9 %) |
| face settle | | | ~260 ms: 15 frames, 5 k px/frame (2.2 %) |
| face drop | 13 frames, 162 k px/frame (70 %) | 480 ms: 27 frames, 25 k px/frame (11 %) | ~515 ms: 29 frames, 11.5–12.5 k px/frame (5.2 %) |

`SIM_ROUND_EVEN=1` (the CO5300's even-aligned flushes) changes the face
numbers by under 1 k px/frame.

`perf` on the serial console prints the same for the last run of each on
hardware: duration, frames and fps, the longest gap between two frames (the
visible stutter), average and worst refresh time (render + flush, the flush
is synchronous), average flush time, and pixels per frame.

**Bottom edge.** The meters screen is not scrollable
(`LV_OBJ_FLAG_SCROLLABLE` off, scrollbar mode off). LVGL's default left it
scrollable, so whenever Clawd walked past an edge (a lurk trip, the
screensaver's walk-off, the attention walk-on) the theme drew a faint grey
4 px scrollbar about 6 px above the bottom edge. The sim's `SIM_EDGE_WATCH=1`
logs every change to the bottom rows and right columns to catch anything
like it.
