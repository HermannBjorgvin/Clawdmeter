#include "sim_platform.h"
#include <SDL.h>
#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

static bool quit = false;

static bool     pwr_down = false;
static uint32_t pwr_down_ms = 0;
static bool     pwr_long_fired = false;
static bool     edge_pressed = false, edge_long = false, edge_released = false;

static int  battery = 87;
static bool charging = false;
// No battery fitted, running on USB (key `u`, or SIM_NO_BATTERY=1 at launch).
static bool no_battery = getenv("SIM_NO_BATTERY") != NULL;

static bool take(bool* f) { bool v = *f; *f = false; return v; }
bool sim_take_pwr_pressed(void)  { return take(&edge_pressed); }
bool sim_take_pwr_long(void)     { return take(&edge_long); }
bool sim_take_pwr_released(void) { return take(&edge_released); }
int  sim_battery_pct(void) { return no_battery ? -1 : battery; }
bool sim_charging(void)    { return !no_battery && charging; }
bool sim_vbus_in(void)     { return no_battery || charging; }
bool sim_should_quit(void) { return quit; }

// Matches the AXP2101 long-press threshold main.cpp's pair gesture expects.
#define PWR_LONG_MS 1500

void sim_set_pwr(bool down) {
    if (down && !pwr_down) {
        pwr_down = true;
        pwr_down_ms = millis();
        pwr_long_fired = false;
    } else if (!down && pwr_down) {
        pwr_down = false;
        if (!pwr_long_fired) edge_pressed = true;
        edge_released = true;
    }
}

void sim_set_battery(int pct, bool chg, bool usb_only) {
    battery = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    charging = chg;
    no_battery = usb_only;
}

void sim_pump(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) quit = true;
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            SDL_Keycode k = e.key.keysym.sym;
            switch (k) {
            case SDLK_ESCAPE: quit = true; break;
            case SDLK_SPACE:  sim_playback_toggle(); break;
            case SDLK_LEFT:   sim_playback_step(-1); break;
            case SDLK_RIGHT:  sim_playback_step(+1); break;
            case SDLK_d:      sim_playback_toggle_link(); break;
            case SDLK_s:      sim_display_screenshot(NULL); break;
            case SDLK_c:      charging = !charging; break;
            case SDLK_u:      no_battery = !no_battery; break;
            case SDLK_MINUS:  battery = battery < 5 ? 0 : battery - 5; break;
            case SDLK_EQUALS: battery = battery > 95 ? 100 : battery + 5; break;
            case SDLK_e:      sim_inject_event('p'); break;
            case SDLK_t:      sim_inject_event('s'); break;
            case SDLK_a:      sim_inject_activity_cycle(); break;
            case SDLK_m:      sim_inject_alert_toggle(); break;
            case SDLK_p:      sim_set_pwr(true); break;
            default:
                if (k >= SDLK_1 && k <= SDLK_9) sim_playback_jump(k - SDLK_1);
                break;
            }
        }
        if (e.type == SDL_KEYUP && e.key.keysym.sym == SDLK_p) sim_set_pwr(false);
    }
    if (pwr_down && !pwr_long_fired && millis() - pwr_down_ms >= PWR_LONG_MS) {
        pwr_long_fired = true;
        edge_long = true;
    }

    // Headless walkthroughs: SIM_SHOTS="ms:path,ms:path,..." saves a
    // screenshot at each time (ascending) and exits after the last.
    {
        static char* shots = NULL;
        static bool  loaded = false;
        if (!loaded) {
            loaded = true;
            const char* v = getenv("SIM_SHOTS");
            if (v) shots = strdup(v);
        }
        while (shots && *shots) {
            char* colon = strchr(shots, ':');
            if (!colon) { shots = NULL; break; }
            const unsigned long at = strtoul(shots, NULL, 10);
            if (millis() < at) break;
            char* comma = strchr(colon + 1, ',');
            char path[256];
            size_t n = comma ? (size_t)(comma - colon - 1) : strlen(colon + 1);
            if (n >= sizeof(path)) n = sizeof(path) - 1;
            memcpy(path, colon + 1, n);
            path[n] = 0;
            sim_display_screenshot(path);
            shots = comma ? comma + 1 : NULL;
            if (!shots) quit = true;
        }
    }

    // SIM_CMDS="ms:cmd;ms:cmd" — scripted input at those times. `pwr` is a
    // PWR short press, `prompt` / `stop` a hook event (keys e / t), `alert`
    // toggles an attention alert / clear (key m), `link` the BLE link (key d); anything
    // else is typed as a serial command line (e.g. `clawd lurk`).
    {
        static char* cmds = NULL;
        static bool  loaded = false;
        if (!loaded) {
            loaded = true;
            const char* v = getenv("SIM_CMDS");
            if (v) cmds = strdup(v);
        }
        while (cmds && *cmds) {
            char* colon = strchr(cmds, ':');
            if (!colon) { cmds = NULL; break; }
            if (millis() < strtoul(cmds, NULL, 10)) break;
            char* semi = strchr(colon + 1, ';');
            if (semi) *semi = 0;
            const char* c = colon + 1;
            printf("[%lu] sim: %s\n", (unsigned long)millis(), c);
            if (strcmp(c, "pwr") == 0)         { edge_pressed = true; edge_released = true; }
            else if (strcmp(c, "prompt") == 0)   sim_inject_event('p');
            else if (strcmp(c, "stop") == 0)     sim_inject_event('s');
            else if (strcmp(c, "alert") == 0)    sim_inject_alert_toggle();
            else if (strcmp(c, "link") == 0)     sim_playback_toggle_link();
            else                                 Serial.feed(c);
            cmds = semi ? semi + 1 : NULL;
        }
    }

    // Headless CI hook: SIM_AUTOSHOT_MS=<ms> → screenshot + exit.
    static long autoshot_ms = -2;
    if (autoshot_ms == -2) {
        const char* v = getenv("SIM_AUTOSHOT_MS");
        autoshot_ms = v ? atol(v) : -1;
    }
    if (autoshot_ms >= 0 && millis() >= (uint32_t)autoshot_ms) {
        const char* p = getenv("SIM_AUTOSHOT_PATH");
        sim_display_screenshot(p ? p : "sim-autoshot.bmp");
        quit = true;
        autoshot_ms = -1;
    }
}

// SIM_TAPS="ms@x,y;ms@x,y+hold" — a finger press at each time, 120 ms
// long unless `+hold` gives its length in ms (a touch hold).
bool sim_scripted_touch(uint16_t* x, uint16_t* y) {
    static const char* taps = NULL;
    static bool loaded = false;
    if (!loaded) { loaded = true; taps = getenv("SIM_TAPS"); }
    if (!taps || !*taps) return false;
    const unsigned long now = millis();
    const char* p = taps;
    while (p && *p) {
        unsigned long at = strtoul(p, NULL, 10);
        const char* atp = strchr(p, '@');
        if (!atp) return false;
        int tx = atoi(atp + 1);
        const char* cm = strchr(atp, ',');
        int ty = cm ? atoi(cm + 1) : 0;
        const char* semi = strchr(p, ';');
        const char* plus = strchr(atp, '+');
        unsigned long len = (plus && (!semi || plus < semi)) ? strtoul(plus + 1, NULL, 10) : 120;
        if (now >= at && now < at + len) { *x = (uint16_t)tx; *y = (uint16_t)ty; return true; }
        p = semi ? semi + 1 : NULL;
    }
    return false;
}
