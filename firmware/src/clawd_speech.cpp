#include "clawd_speech.h"
#include <Arduino.h>
#include <esp_random.h>
#include <stdarg.h>
#include <string.h>

// ── Bubble object + state ────────────────────────────────────────────────────
static lv_obj_t* s_bubble = nullptr;
static lv_obj_t* s_label  = nullptr;
static lv_obj_t* s_mascot = nullptr;          // anchor; bubble hides while it's hidden
static uint32_t  s_hide_ms = 0;               // lv_tick when the bubble should vanish (0 = hidden)

// ── Autonomous chatter ───────────────────────────────────────────────────────
static ClawdChattiness s_chat = CLAWD_BALANCED;
static void (*s_ctx_provider)(ClawdContext*) = nullptr;
static uint32_t s_next_auto_ms = 0;           // lv_tick of the next unprompted quip

// Schedule the next self-initiated quip: a base gap by mode + up to +50% jitter,
// so he doesn't chatter on a metronome.
static void schedule_next_auto(uint32_t now) {
    uint32_t base = (s_chat == CLAWD_CHATTY) ? 22000 : 80000;   // ~22s vs ~80s
    s_next_auto_ms = now + base + (uint32_t)(esp_random() % (base / 2 + 1));
}

void clawd_speech_init(lv_obj_t* parent, lv_obj_t* mascot, const ClawdSpeechStyle* style) {
    s_mascot = mascot;
    s_bubble = lv_obj_create(parent);
    lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_bubble, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_bubble, style->bubble_bg, 0);
    lv_obj_set_style_bg_opa(s_bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_bubble, 12, 0);
    lv_obj_set_style_border_width(s_bubble, 0, 0);
    lv_obj_set_style_pad_left(s_bubble, 12, 0);
    lv_obj_set_style_pad_right(s_bubble, 12, 0);
    lv_obj_set_style_pad_top(s_bubble, 8, 0);
    lv_obj_set_style_pad_bottom(s_bubble, 8, 0);

    s_label = lv_label_create(s_bubble);
    lv_label_set_long_mode(s_label, LV_LABEL_LONG_WRAP);   // long lines wrap + grow tall
    lv_obj_set_style_max_width(s_label, style->max_width, 0);
    lv_obj_set_style_text_font(s_label, style->font, 0);
    lv_obj_set_style_text_color(s_label, style->text_color, 0);
    lv_label_set_text(s_label, "");
}

void clawd_speech_hide(void) {
    s_hide_ms = 0;
    if (s_bubble) lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
}

void clawd_speech_set_chattiness(ClawdChattiness mode) {
    s_chat = mode;
    schedule_next_auto(lv_tick_get());   // start the next-gap clock fresh on a mode change
}
ClawdChattiness clawd_speech_get_chattiness(void) { return s_chat; }
void clawd_speech_set_context_provider(void (*provider)(ClawdContext*)) { s_ctx_provider = provider; }

void clawd_speech_tick(void) {
    uint32_t now = lv_tick_get();
    if (s_hide_ms && now >= s_hide_ms) clawd_speech_hide();

    // Autonomous chatter: when idle (no bubble up) and the mascot is on screen,
    // Clawd occasionally speaks unprompted; QUIET disables it.
    if (s_chat != CLAWD_QUIET && s_ctx_provider && s_hide_ms == 0 &&
        s_mascot && !lv_obj_has_flag(s_mascot, LV_OBJ_FLAG_HIDDEN) &&
        now >= s_next_auto_ms) {
        ClawdContext c; s_ctx_provider(&c);
        clawd_quip(&c);                  // -> clawd_speak() reschedules the next gap
    }
}

void clawd_speak(const char* text) {
    if (!s_bubble || !text || !text[0]) return;
    if (s_mascot && lv_obj_has_flag(s_mascot, LV_OBJ_FLAG_HIDDEN)) return;  // nothing to attach to
    lv_label_set_text(s_label, text);
    lv_obj_clear_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(s_bubble);                        // size to the new text before aligning
    lv_obj_align_to(s_bubble, s_mascot, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);
    // Dwell scales with length from the first character (short quips are still
    // comfortable to read, long ones get more time): ~2.5s base + ~70ms/char
    // (~14 chars/sec reading), capped so the long-winded one doesn't overstay.
    uint32_t chars = (uint32_t)strlen(text);
    uint32_t dwell = 2500 + chars * 70;
    if (dwell > 15000) dwell = 15000;
    uint32_t now = lv_tick_get();
    s_hide_ms = now + dwell;
    schedule_next_auto(now);   // any speak (tap, auto, or feedback) pushes the next auto-quip out
}

void clawd_speak_fmt(const char* fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    clawd_speak(buf);
}

// ── Quip repository ──────────────────────────────────────────────────────────
// A weighted candidate pool, rebuilt from the ClawdContext on every tap. Higher
// weight = more likely. Situational lines outweigh generic flavor, so Clawd
// mostly reacts to what's happening but still surprises now and then.
#define QUIP_MAX 16
#define QUIP_LEN 256
static char s_cand[QUIP_MAX][QUIP_LEN];
static int  s_wt[QUIP_MAX];
static int  s_n;

// Add a candidate line. printf-style; the bubble font has ASCII + the usual
// punctuation but NO em dash / ellipsis — use '-' and '...'.
static void add(int weight, const char* fmt, ...) {
    if (s_n >= QUIP_MAX) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(s_cand[s_n], QUIP_LEN, fmt, ap);
    va_end(ap);
    s_wt[s_n] = weight; s_n++;
}

// ═══════════════════════════════════════════════════════════════════════════
// EDIT HERE to change what Clawd says. Each add() is one candidate line; guard
// it with any condition on `c` and give it a weight. Two phrasings per situation
// keeps repeats from feeling stale. Keep generic gags low-weight so they land as
// treats. `si`/`wi` are the rounded session/weekly %, `c->burn_rate` is %/hr.
// ═══════════════════════════════════════════════════════════════════════════
static void clawd_collect_quips(const ClawdContext* c) {
    s_n = 0;
    const int si = (int)(c->session_pct + 0.5f);
    const int wi = (int)(c->weekly_pct + 0.5f);

    // session load — a line for every level, so he's always aware
    if      (c->session_pct >= 92) { add(9, "%d%%?! Easy, champ - go breathe.", si);
                                     add(8, "%d%% already?! Step away slowly.", si); }
    else if (c->session_pct >= 75) { add(6, "%d%% and climbing. Pace yourself.", si);
                                     add(6, "%d%% deep. Deep breaths, too.", si); }
    else if (c->session_pct >= 40) { add(4, "%d%% this session. Solid pace.", si);
                                     add(4, "%d%% in. Right down the middle.", si); }
    else if (c->session_pct >= 16) { add(4, "%d%% in - just warming up.", si);
                                     add(4, "Only %d%% so far. Cruising.", si); }
    else if (c->session_pct >= 4)  { add(4, "Just %d%% in. Plenty of runway.", si);
                                     add(4, "%d%%. Barely getting started.", si); }
    else                           { add(4, "Fresh 5-hour window. Go wild.");
                                     add(4, "0%%. A blank canvas."); }

    // burn rate
    if (c->burn_known) {
        int rw = (int)c->burn_rate, rf = (int)((c->burn_rate - (float)rw) * 10.0f + 0.5f);
        if (rf > 9) { rw++; rf = 0; }
        if      (c->burn_rate >= 18) { add(7, "%d.%d%%/hr - you're COOKING. See the flame?", rw, rf);
                                       add(6, "%d.%d%%/hr! Slow down, speedster.", rw, rf); }
        else if (c->burn_rate >= 8)  { add(5, "Steady ~%d.%d%%/hr. Nice rhythm.", rw, rf);
                                       add(4, "~%d.%d%%/hr. Good clip.", rw, rf); }
        else if (c->burn_rate >= 1)  { add(3, "Ticking along, ~%d.%d%%/hr.", rw, rf);
                                       add(3, "~%d.%d%%/hr. Nice and measured.", rw, rf); }
        else                         { add(3, "Barely sipping tokens. Zen mode.");
                                       add(3, "Practically idle. Very zen."); }
    }

    // reset countdown
    if (c->session_reset_min > 0 && c->session_reset_min <= 20)
        add(12, "New session in %dm - almost refueled.", c->session_reset_min);

    // weekly load
    if      (c->weekly_pct >= 88) add(12, "Heads up - %d%% of the week's gone.", wi);
    else if (c->weekly_pct >= 40) add(6,  "%d%% into the week so far.", wi);
    else if (c->weekly_pct >= 9)  add(5,  "%d%% of the week used. Cruising.", wi);
    else                          add(5,  "Whole week ahead of us.");

    // connection
    if (!c->connected) add(9, "Lost the daemon... talking to myself here.");

    // flavor — occasional (gated + low weight) so situational lines dominate
    if (c->costume && (esp_random() & 1)) add(3, "How's the %s look on me?", c->costume);
    if      (c->hour >= 0 && c->hour < 5) add(6, "Burning the midnight oil? Same.");
    else if (c->hour >= 5 && c->hour < 9) add(5, "Morning! Coffee, then code?");
    else if (c->hour >= 22)               add(6, "Late one tonight, huh?");

    // generic gags — rare treats
    add(1, "You're absolutely right!");
    add(1, "*happy crab noises*");
    add(1, "You raise a genuinely excellent point, and I want to make sure I "
           "address it with the care it deserves: having carefully weighed the "
           "tradeoffs, considered the edge cases, consulted my training, and "
           "reflected deeply on the matter... you're absolutely right!");
}
// ═══════════════════════════════════════════════════════════════════════════

void clawd_quip(const ClawdContext* ctx) {
    clawd_collect_quips(ctx);
    int total = 0; for (int i = 0; i < s_n; i++) total += s_wt[i];
    if (total <= 0) return;
    static char last[QUIP_LEN] = "";
    const char* pick = s_cand[0];
    for (int tries = 0; tries < 3; tries++) {              // avoid an immediate exact repeat
        int r = (int)(esp_random() % (uint32_t)total), acc = 0;
        for (int i = 0; i < s_n; i++) { acc += s_wt[i]; if (r < acc) { pick = s_cand[i]; break; } }
        if (strcmp(pick, last) != 0) break;
    }
    strncpy(last, pick, sizeof(last) - 1); last[sizeof(last) - 1] = 0;
    clawd_speak(pick);
}
