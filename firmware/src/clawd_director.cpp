#include "clawd_director.h"
#include "clawd_policy.h"
#include "clawd_anim.h"
#include "mascot.h"
#include "performer.h"
#include "face.h"
#include "ui.h"
#include "idle.h"
#include "minimal_mode.h"
#include "hal/board_caps.h"
#include "usage_rate.h"
#include <Arduino.h>
#include <lvgl.h>
#include <string.h>
#include <stdlib.h>

// ─── Beats ───────────────────────────────────────────────────────────────────
// Each beat is a short script of steps. A step issues commands to the actors
// (corner + full-size sprite, performer, face) and the next step runs once
// they have all settled and any explicit pause has passed. Interruptions
// (tap, prompt, attention) jump to a transition beat that gets back to the
// meters without a hard cut.
enum Beat {
    B_HOME,          // meters, corner Clawd idling on his own
    B_CORNER,        // a commanded corner act (notice, wave, joy) or walk home
    B_SHOW,          // notice → walks off → performs on the meters → home
    B_SCREENSAVER,   // walk off → face rests, dips into calm performances
    B_FACE_HOME,     // face drops into the meters, he hurries to his seat
    B_RUN_HOME,      // the performance stops, he runs off, the corner walks home
    B_ATTENTION,     // the big Clawd fetches you
    B_PEEK,          // a tap on the meters: hop, duck out, the face comes up curious for a while
};
static const char* const BEAT_NAMES[] = {
    "home", "corner", "show", "screensaver", "face-home", "run-home", "attention", "peek",
};

static Beat     beat = B_HOME;
static uint8_t  step = 0;
static uint32_t wait_until = 0;
static bool     performing = false;   // he's out as the performer on the meters
static bool     notice_wake = false;  // B_FACE_HOME: blink first (he noticed you)

// Attention
static bool     attention_on = false;
static bool     attention_dismissed = false;  // ended here; the app hasn't heard
static bool     pairing_touch = false;        // director_set_pairing_touch
static uint32_t attention_since = 0;
static bool     attn_anim_done = false;       // Clawd's part of the alert is over; it stays up
static AttnFrom attn_from = ATTN_FROM_HOME;   // what it interrupted
static uint32_t attn_mark = 0;                // last_engagement right after the alert
static bool     attn_engaged = false;         // you showed up since the alert (sticky across refreshes)
static bool     resume_screensaver = false;   // decided when attention ends

// A notice / wave waiting for him to hurry home from a lurk trip.
static const char* react_anim = NULL;
static uint8_t     react_times = 0;
static uint32_t    react_at = 0;
static bool        trace = false;

// Situation
static ShowGate    show_gate;
static TurnTracker turn;
static uint32_t    last_engagement = 0;
static bool        window_not_started = false;
static bool        have_pct = false;
static float       last_pct = 0;
static bool        joy_pending = false;
static uint32_t    joy_at = 0;
static bool        minimal = false;
static ClawdRng    rng;

// A tap on the meters (the aquarium)
static int         peek_x = 0;                // where the peek's tap landed
static uint32_t    peek_up_at = 0;            // the peek's face started rising
static lv_indev_t* touch_indev = NULL;

static uint32_t now_ms(void) { return millis(); }

static void go(Beat b) {
    if (b != beat) Serial.printf("[%lu] director: %s -> %s\n", (unsigned long)now_ms(),
                                 BEAT_NAMES[beat], BEAT_NAMES[b]);
    beat = b;
    step = 0;
    wait_until = now_ms();
    mascot_autonomy(b == B_HOME && !minimal && clawd_tuning().idle_acts);
}

void director_set_trace(bool on) { trace = on; }

static void next_step(uint32_t pause_ms = 0) {
    step++;
    if (trace)
        Serial.printf("[%lu] director:   %s step %u\n", (unsigned long)now_ms(), BEAT_NAMES[beat], step);
    wait_until = now_ms() + pause_ms;
}

static bool settled(uint32_t now) {
    return (int32_t)(now - wait_until) >= 0 && !face_moving() && !performer_busy() &&
           !mascot_busy();
}

static void engage(void) { last_engagement = now_ms(); }

// The performer walks on from the left edge, where the corner Clawd just
// left (one Clawd: only once he's away and no face is up).
static void perform_enter(uint16_t speed_pct) {
    performer_place_offstage(PERFORMER_LEFT);
    performer_walk_home(speed_pct);
    performing = true;
}

// Back on the meters after a transition: attention takes over if it's
// waiting; an alert that ended mid-transition with nobody around puts the
// screensaver back; otherwise he walks back to his seat from the left.
static void arrive_meters(void) {
    if (attention_on) { go(B_ATTENTION); return; }
    if (resume_screensaver) {
        resume_screensaver = false;
        if (!minimal && ui_meters_live()) { go(B_SCREENSAVER); return; }
    }
    mascot_return(-1, 150);
    go(B_CORNER);
}

// A corner reaction (noticing, the long-turn wave). If he's out on a lurk
// trip he cuts it short and hurries back, then reacts (anticipation: the
// reaction is never just dropped). Only on the meters with nothing staged.
static void react(const char* anim, uint8_t times) {
    if (!clawd_tuning().reactions) return;
    if (beat != B_HOME && beat != B_CORNER) return;
    if (mascot_act(anim, times)) { react_anim = NULL; go(B_CORNER); return; }
    if (!mascot_hurry_home()) return;
    react_anim = anim;
    react_times = times;
    react_at = now_ms();
    if (beat != B_CORNER) go(B_CORNER);
}

// "Show me the meters": the fastest no-cut path from wherever we are.
static void to_meters(bool noticed) {
    if (beat == B_FACE_HOME || beat == B_RUN_HOME) return;   // already on the way
    face_set_resting(false);
    if (face_visible()) {
        notice_wake = noticed;
        go(B_FACE_HOME);
    } else if (performing) {
        go(B_RUN_HOME);
    } else if (beat == B_SHOW || beat == B_SCREENSAVER) {
        // Still on the meters (pointing / walking off): turn around.
        if (attention_on) { go(B_ATTENTION); return; }
        mascot_return(-1, 150);
        go(B_CORNER);
    }
}

// ─── Attention ───────────────────────────────────────────────────────────────
// Clawd's part of an alert: he points and waves for the attention
// animation's time (clawd_attn_anim), then sits in his seat while the alert
// itself stays up until a tap, a clear or its timeout.
static bool attn_anim_running(uint32_t now) {
    return !attn_anim_done &&
           (uint32_t)(now - attention_since) < clawd_attn_anim_ms(clawd_attn_anim(), clawd_tuning().alert_timeout_ms);
}

static void end_attention(AttnEnd how) {
    if (!attention_on) return;
    attention_on = false;
    if (attention_end_tells_host(how)) attention_dismissed = true;
    const bool engaged = attn_engaged || (int32_t)(last_engagement - attn_mark) > 0;
    resume_screensaver = attention_resumes_screensaver(attn_from, how, engaged);
    ui_set_alert_text(NULL);
    mascot_attention(false, !resume_screensaver);
    Serial.printf("[%lu] director: attention over (%s)%s\n", (unsigned long)now_ms(),
                  how == ATTN_END_TAP ? "tap" : how == ATTN_END_CLEAR ? "cleared" : "timeout",
                  resume_screensaver ? ", back to the screensaver" : "");
    if (beat == B_ATTENTION && step == 0) next_step();
}

void director_alert(const char* message) {
    if (!attention_on) {
        // Remember what it interrupts, so it can return there. A new alert
        // while the last one is still leaving for the screensaver inherits
        // that origin (nobody has shown up in between).
        const bool leaving_for_screensaver = beat == B_ATTENTION && resume_screensaver;
        attn_from = (beat == B_SCREENSAVER || leaving_for_screensaver) ? ATTN_FROM_SCREENSAVER
                  : (beat == B_SHOW)                                  ? ATTN_FROM_SHOW
                                                                      : ATTN_FROM_HOME;
        attn_engaged = false;
        resume_screensaver = false;
        react_anim = NULL;                    // the big Clawd outranks a corner reaction
    } else if ((int32_t)(last_engagement - attn_mark) > 0) {
        attn_engaged = true;                  // a refresh must not forget that you showed up
    }
    engage();
    attn_mark = last_engagement;
    attention_since = now_ms();
    attn_anim_done = false;                   // a new alert gets its animation again
    ui_set_alert_text(message);
    if (attention_on) return;                 // refresh text + timers only
    attention_on = true;
    if (beat == B_ATTENTION) { step = 0; return; }
    // Attention never waits: interrupt whatever is on.
    if (face_visible() || performing) {
        face_set_resting(false);
        notice_wake = false;
        if (face_visible()) go(B_FACE_HOME);
        else                go(B_RUN_HOME);
    } else {
        go(B_ATTENTION);
    }
}

void director_alert_clear(void) { end_attention(ATTN_END_CLEAR); }

bool director_take_dismissed(void) {
    const bool d = attention_dismissed;
    attention_dismissed = false;
    return d;
}

// ─── Inputs ──────────────────────────────────────────────────────────────────
// Peek speeds: the hop and the duck-out are the acknowledgement, not the
// point, so they're quick and the face isn't kept waiting.
#define PEEK_HOP_PCT    250
#define PEEK_LEAVE_PCT  500

// A tap while he's on the meters: tapping the aquarium always brings the face
// up (clawd_tap_react). Seated, he hops first (the act opens on its crouch, so
// he reacts on the next frame); otherwise he heads straight out.
static void tap_home(int x) {
    TapInput in;
    in.on_meters = beat == B_HOME || beat == B_CORNER;
    in.minimal = minimal;
    in.corner = mascot_free() ? CORNER_FREE : mascot_out() ? CORNER_OUT : CORNER_BUSY;
    TapReact r = clawd_tap_react(&in);
    if (r == TAP_PEEK_HOP && !mascot_act("jumping", 1, true, PEEK_HOP_PCT)) r = TAP_PEEK;
    if (r == TAP_PEEK) mascot_leave(PEEK_LEAVE_PCT);
    if (r != TAP_NONE) {
        react_anim = NULL;               // he's off to the glass; a queued corner reaction is moot
        peek_x = x;
        go(B_PEEK);
    }
    Serial.printf("[%lu] director: tap at x %d -> %s\n", (unsigned long)now_ms(), x,
                  r == TAP_PEEK_HOP ? "peek (hop)" : r == TAP_PEEK ? "peek (heads out)" : "absorbed");
}

void director_tap(int x) {
    engage();
    if (attention_on) { end_attention(ATTN_END_TAP); return; }   // a dismissing tap does nothing else
    resume_screensaver = false;                   // "show me the meters" wins
    if (minimal) return;
    if (beat == B_HOME || beat == B_CORNER) {
        if (pairing_touch) return;                // the hint is the control now
        tap_home(x);
        return;
    }
    if (beat == B_PEEK) {
        // The face is up (or coming up) looking at you: he blinks and drops
        // right away. Before it shows (hop, ducking out) the tap is
        // absorbed: nothing queues.
        const bool drop = face_visible();
        if (drop) to_meters(true);
        Serial.printf("[%lu] director: tap at x %d -> %s\n", (unsigned long)now_ms(), x,
                      drop ? "peek over" : "absorbed");
        return;
    }
    to_meters(false);
}

void director_set_pairing_touch(bool on) { pairing_touch = on; }

bool director_button(bool main_button) {
    engage();
    if (main_button && attention_on && !board_caps().has_touch) {
        end_attention(ATTN_END_TAP);          // nothing to tap: BOOT dismisses it
        return true;
    }
    resume_screensaver = false;
    if (beat == B_SCREENSAVER) to_meters(false);
    return false;
}

void director_set_attn_anim(int seconds) {
    clawd_set_attn_anim(seconds);             // a running alert follows it on the next tick
}

void director_set_liveliness(int level) {
    clawd_set_liveliness(level);
    const ClawdTuning& t = clawd_tuning();
    if (!t.reactions) { react_anim = NULL; joy_pending = false; }
    if (!t.screensaver) resume_screensaver = false;
    if ((beat == B_SHOW && !t.shows) || (beat == B_SCREENSAVER && !t.screensaver)) to_meters(false);
    mascot_autonomy(beat == B_HOME && !minimal && t.idle_acts);
}

static void indev_cb(lv_event_t* e) {
    (void)e;
    lv_point_t p = { 0, 0 };
    if (touch_indev) lv_indev_get_point(touch_indev, &p);
    director_tap(p.x);
}

void director_on_payload(const UsageData* d, const SignalsUpdate* sig) {
    if (!d->valid || !d->ok) return;
    window_not_started = !d->enterprise && d->session_reset_mins < 0;

    // The slow clock moving counts as you being around.
    if (!sig->replay) {
        if (have_pct && d->session_pct > last_pct + 0.01f) {
            engage();
            if (beat == B_SCREENSAVER) to_meters(true);
        }
        have_pct = true;
        last_pct = d->session_pct;
    }

    if (sig->session_reset && !minimal && clawd_tuning().reactions) {
        // Fresh window: jumping happy twice, the same moment as the chime if
        // he's home, else when he gets back. Never on replays (signals only
        // reports real samples). It outranks the notice / wave below.
        if (beat == B_HOME && mascot_act("jumping happy", 2)) go(B_CORNER);
        else {
            joy_pending = true;
            joy_at = now_ms();
            if (beat == B_HOME) mascot_hurry_home();   // out on a trip: come back for it
        }
    }

    if (sig->event_fired) {
        engage();
        const bool wave = turn_on_event(&turn, sig->event, now_ms(), &clawd_tuning());
        if (minimal) return;
        if (sig->event == 'p' || sig->event == 'b') {
            resume_screensaver = false;       // you're back
            if (beat == B_SCREENSAVER) {
                to_meters(true);              // he notices, the face drops
            } else {
                // Anticipation: he turns and points before anything else.
                react("pointing", 1);
            }
        } else if (sig->event == 's' && wave) {
            react("waving", 1);
        }
    }
}

// ─── Minimal mode ────────────────────────────────────────────────────────────
static void apply_minimal(bool on) {
    minimal = on;
    react_anim = NULL;                        // a queued corner reaction is moot now
    resume_screensaver = false;
    face_hide_now();
    performer_place_offstage(PERFORMER_LEFT);
    performing = false;
    mascot_set_minimal(on);
    // An alert that's still up stays in its beat, so its timeout and the
    // dismissing tap keep working across the switch.
    beat = attention_on ? B_ATTENTION : B_HOME;
    step = 0;
    mascot_autonomy(!on && !attention_on);
    if (attention_on && attn_anim_running(now_ms())) mascot_attention(true);
    Serial.printf("director: minimal %s\n", on ? "on" : "off");
}

// ─── Init ────────────────────────────────────────────────────────────────────
void director_init(void) {
    face_init();
    show_gate_clear(&show_gate);
    turn_clear(&turn);
    clawd_rng_seed(&rng, now_ms() ^ 0xD1EC7u);
    last_engagement = now_ms();
    apply_minimal(minimal_mode());
    go(B_HOME);
    // Every press on the panel, wherever it lands, is a tap for the
    // director. PRESSED (not CLICKED) so the reaction starts on the next frame.
    touch_indev = lv_indev_get_next(NULL);
    if (touch_indev) lv_indev_add_event_cb(touch_indev, indev_cb, LV_EVENT_PRESSED, NULL);
}

DirectorRoom director_room(void) { return face_visible() ? ROOM_FACE : ROOM_METERS; }

const char* director_beat_name(void) { return BEAT_NAMES[beat]; }

// ─── Beat scripts ────────────────────────────────────────────────────────────
// Without the full-size performer (no PSRAM) the show is a corner act from
// the show's group: he dances, or jumps for joy when usage really burns.
static const char* corner_show_act(int group) { return group >= 3 ? "jumping happy" : "dancing"; }

static void run_show(uint32_t now) {
    if (step == 0) {
        // Noticing: he points at the bar first. If he's mid-act, let it
        // finish; if he's already up and about (a lurk trip), he heads
        // straight out instead.
        if (mascot_acting()) return;
        show_gate_mark_shown(&show_gate, now);
        if (mascot_seated())              { mascot_act("pointing", 1); next_step(); }
        else if (performer_available())   { mascot_leave(150); step = 2; wait_until = now; }
        else                              { go(B_HOME); }   // corner-only: he's out, skip it
        return;
    }
    if (!settled(now)) return;
    const ClawdTuning& t = clawd_tuning();
    // The trigger lifted the live group to Heavy; read the usage-only base
    // group (floored at Active) so the whole repertoire shows up.
    const int g = clawd_show_group(usage_rate_base_group());
    if (!performer_available()) {
        switch (step) {
        case 1:  mascot_act(corner_show_act(g), 2); next_step(); break;
        case 2:  mascot_act("waving", 1);           next_step(); break;
        default: go(B_HOME);                                     break;
        }
        return;
    }
    switch (step) {
    case 1:  // steps out through the left edge
        mascot_leave(150);
        next_step();
        break;
    case 2:  // gone: he walks back on, full size, in front of the meters
        if (!mascot_away()) return;
        perform_enter(150);
        next_step();
        break;
    case 3:
        performer_perform_group(g, t.show_scene_ms, false);
        next_step();
        break;
    case 4:  // show over: a wave
        performer_perform(clawd_anim_index("waving"), 0);
        next_step();
        break;
    case 5:
        performer_walk_off(PERFORMER_LEFT, 150);
        next_step();
        break;
    case 6:  // off the edge: the corner Clawd comes back in from there
        if (performer_visible()) return;
        performing = false;
        mascot_return(-1, 100);
        next_step();
        break;
    default:
        go(B_HOME);
        break;
    }
}

static uint32_t calm_at = 0;

static void run_screensaver(uint32_t now) {
    const ClawdTuning& t = clawd_tuning();
    if (!ui_meters_live()) { to_meters(false); return; }   // link dropped: show the hint
    if (step == 3) {
        // Resting: dip into a calm performance now and then (also breaks up
        // the flat orange field for burn-in). One Clawd: the face drops onto
        // the empty meters, he walks on only once it's gone, and walks off
        // before it comes back.
        if ((int32_t)(now - calm_at) < 0 || face_busy()) return;
        face_set_resting(false);
        next_step(200);
        return;
    }
    if (step == 0) {
        // Gets up to check on you, from whatever he was doing: an idle act
        // ends, a walk heads for the nearest edge, a peek finishes first.
        mascot_leave(100);
        next_step();
        return;
    }
    if (!settled(now)) return;
    switch (step) {
    case 1:
        if (!mascot_away()) return;
        face_rise();
        next_step();
        break;
    case 2:
        face_set_resting(true);
        calm_at = now + clawd_rng_range(&rng, t.calm_min_ms, t.calm_max_ms);
        step = 3;
        break;
    case 4:
        face_drop();
        next_step();
        break;
    case 5:  // the face is gone: he strolls on (corner-only: back to his seat)
        if (performer_available()) perform_enter(100);
        else                       mascot_return(-1, 100);
        next_step();
        break;
    case 6:  // a calm scene (corner-only: he just sits a while)
        if (performer_available()) { performer_perform_group(0, t.calm_scene_ms, true); next_step(); }
        else                       next_step(t.calm_scene_ms);
        break;
    case 7:
        if (performing) performer_walk_off(PERFORMER_LEFT, 100);
        else            mascot_leave(100);
        next_step();
        break;
    case 8:  // off screen: the face comes back up
        if (performer_visible() || !mascot_away()) return;
        performing = false;
        face_rise();
        step = 2;
        break;
    }
}

static void run_face_home(uint32_t now) {
    switch (step) {
    case 0:
        if (notice_wake && !face_moving()) {
            face_blink(false);              // noticed you: a blink, then down
            next_step(260);
        } else {
            next_step();
        }
        break;
    case 1:
        if ((int32_t)(now - wait_until) < 0) return;
        mascot_place_away();
        face_drop();
        next_step();
        break;
    default:
        if (!settled(now)) return;
        arrive_meters();
        break;
    }
}

// The performance stops for you: the scene releases to the idle pose on the
// spot, a beat, and he runs off left, picking up speed; the corner Clawd
// walks home from there.
static void run_run_home(uint32_t now) {
    switch (step) {
    case 0:  // the scene releases to the idle pose, right now
        performer_release();
        next_step(200);             // a beat: he registers it (anticipation)
        break;
    case 1:  // then gets going: 1.5x ramping to 3x
        if (!settled(now)) return;
        performer_run_off(PERFORMER_LEFT, 150, 300, 400);
        next_step();
        break;
    default:
        if (!settled(now) || performer_visible()) return;
        performing = false;
        arrive_meters();
        break;
    }
}

static void run_attention(uint32_t now) {
    if (step == 0) {
        if (attn_anim_running(now)) {
            mascot_attention(true);
        } else {
            if (!attn_anim_done) {            // the animation is over (or off): home to his seat
                attn_anim_done = true;
                mascot_attention(false, true);
                mascot_hurry_home();
            }
            if (mascot_away()) mascot_return(-1, 150);   // arrived here with him out of sight
        }
        if ((uint32_t)(now - attention_since) >= clawd_tuning().alert_timeout_ms)
            end_attention(ATTN_END_TIMEOUT);
        return;
    }
    // Over: wait for the big one to leave and the small one to sit down (or,
    // when the screensaver resumes, to stay out of sight).
    if (mascot_in_attention() || mascot_busy()) return;
    if (resume_screensaver) {
        resume_screensaver = false;
        if (!minimal && ui_meters_live()) { go(B_SCREENSAVER); return; }
    }
    if (mascot_away()) {              // was told to stay out, then you showed up
        mascot_return(-1, 150);
        go(B_CORNER);
        return;
    }
    go(B_HOME);
}

// A tap on the meters (tap_home already started the startled hop, or sent
// him out). One Clawd: he ducks out through the nearest edge (his seat is by
// the left edge, so it's a few steps) before the face comes up. The face then
// stays, resting and curious, for peek_curious_ms, or until you tap it
// (director_tap → the acknowledging blink, the drop, and he walks back to his
// seat).
static void run_peek(uint32_t now) {
    if (!settled(now)) return;
    const ClawdTuning& t = clawd_tuning();
    switch (step) {
    case 0:  // the hop has landed: he ducks out, quick
        mascot_leave(PEEK_LEAVE_PCT);
        next_step();
        break;
    case 1:  // gone: the face rises, curious about the tap
        if (!mascot_away()) return;
        face_rise_toward(peek_x);
        peek_up_at = now;
        next_step();
        break;
    case 2:  // up: he looks where you tapped
        face_look_at_x(peek_x, clawd_rng_range(&rng, t.peek_look_min_ms, t.peek_look_max_ms));
        next_step(320);
        break;
    case 3:  // ...and blinks at it (sometimes twice)
        face_blink(clawd_rng_chance(&rng, t.peek_double_pct));
        next_step();
        break;
    case 4:  // the look comes back to you, and he stays: resting on the glass
        if (face_busy()) return;
        face_set_resting(true);
        next_step();
        break;
    default:  // curious for a while, then back to his seat on his own
        if (peek_hold(peek_up_at, now, &t) == PEEK_HOLD_DONE) {
            Serial.printf("[%lu] director: peek over (curiosity)\n", (unsigned long)now);
            to_meters(false);
        }
        break;
    }
}

static void run_corner(uint32_t now) {
    if (react_anim) {
        if (!reaction_still_due(react_at, now, &clawd_tuning())) {
            react_anim = NULL;                       // never made it home in time
        } else {
            if (!mascot_seated() && !mascot_acting()) return;   // still hurrying back
            if (!mascot_act(react_anim, react_times)) return;   // a commanded act first
            react_anim = NULL;
            return;
        }
    }
    if (settled(now)) go(B_HOME);
}

static void run_home(uint32_t now) {
    if (minimal) return;
    if (idle_is_asleep()) return;                 // nobody's looking
    if (joy_pending) {
        if ((uint32_t)(now - joy_at) > clawd_tuning().joy_pending_ms) joy_pending = false;
        else if (mascot_act("jumping happy", 2)) { joy_pending = false; go(B_CORNER); return; }
    }
    const bool live = ui_meters_live();
    QuietInput q = { now, last_engagement, signals_hooks_seen(), window_not_started, live };
    if (screensaver_ready(&q, &clawd_tuning())) { go(B_SCREENSAVER); return; }
    if (live && signals_hooks_seen() && show_gate_ready(&show_gate, now, &clawd_tuning()))
        go(B_SHOW);
}

void director_tick(void) {
    const uint32_t now = now_ms();
    const int activity = signals_activity();
    show_gate_update(&show_gate, now, activity, &clawd_tuning());
    // Tool calls flowing (PostToolUse) are hook events too: you're working.
    if (activity > 0) last_engagement = now;
    face_tick();

    switch (beat) {
    case B_HOME:        run_home(now); break;
    case B_CORNER:      run_corner(now); break;
    case B_SHOW:        run_show(now); break;
    case B_SCREENSAVER: run_screensaver(now); break;
    case B_FACE_HOME:   run_face_home(now); break;
    case B_RUN_HOME:    run_run_home(now); break;
    case B_ATTENTION:   run_attention(now); break;
    case B_PEEK:        run_peek(now); break;
    }
}

// ─── QA ──────────────────────────────────────────────────────────────────────
bool director_debug(const char* name) {
    if (strcmp(name, "home") == 0) {
        // "Show me the meters" (a tap anywhere but home, without the peek).
        engage();
        if (attention_on) { end_attention(ATTN_END_TAP); return true; }
        resume_screensaver = false;
        if (!minimal) to_meters(false);
        return true;
    }
    if (strcmp(name, "attention") == 0)   { director_alert("Clawdmeter test alert"); return true; }
    if (minimal) return true;             // nothing else exists in minimal mode
    if (strcmp(name, "joy") == 0) {
        joy_pending = true;
        joy_at = now_ms();
        return true;
    }
    if (strncmp(name, "peek", 4) == 0) {  // the peek now, as a tap at x would; `clawd peek <x>`
        if (beat != B_HOME && beat != B_CORNER) return false;
        const int x = name[4] == ' ' ? atoi(name + 5) : lv_display_get_horizontal_resolution(NULL) * 3 / 4;
        engage();
        resume_screensaver = false;
        tap_home(x);
        return beat == B_PEEK;
    }
    if (strcmp(name, "lurk") == 0) {      // a lurk trip now (to QA cutting it short)
        if (beat != B_HOME) return false;
        return mascot_lurk();
    }
    const bool show = strcmp(name, "show") == 0;
    if (!show && strcmp(name, "screensaver") != 0) return false;
    if (beat != B_HOME) return false;     // finish the current beat first (`clawd home`)
    go(show ? B_SHOW : B_SCREENSAVER);
    return true;
}
