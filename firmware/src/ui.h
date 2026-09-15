#pragma once
#include "data.h"
#include "ble.h"
#include "wifi_net.h"

enum screen_t {
    SCREEN_SPLASH,
    SCREEN_CLOCK,
    SCREEN_SENSOR,
    SCREEN_SENSOR_GRAPH,   // 24 h trend graphs from the on-device sensor
    SCREEN_USAGE,
    SCREEN_COPILOT,
    SCREEN_SYSINFO,
    SCREEN_VSCODE,
    SCREEN_BLUETOOTH,
    SCREEN_CI,
    SCREEN_TODAY,
    SCREEN_AURORA,
    SCREEN_COUNT,
};

void ui_init(void);
void ui_update(const UsageData* data);
void ui_update_copilot(const CopilotData* data);
void ui_update_sysinfo(const SysInfoData* data);
void ui_update_vscode(const VscodeData* data);
void ui_update_env(const EnvData* data);         // {"src":"env",...} — clock + weather
void ui_update_aurora(const AuroraData* data);   // {"src":"aurora",...} — aurora borealis forecast
void ui_update_sensor(bool present, float temp_c, float pressure_hpa,
                       bool has_humidity, float humidity_pct); // env sensor (BME280/BMP180), on-device
void ui_update_ci(const CiData* data);           // {"src":"ci",...}  — CI + review queue
void ui_update_today(const TodayData* data);      // {"src":"sum",...}
void ui_update_act(const char* state, int agents); // {"src":"act",...} — Claude activity
void ui_update_daemon_state(const char* state);  // {"src":"status","state":...}

// Accessors for the HTTP /api/state endpoint (web_server.cpp) — expose the
// bits of state that live only inside ui.cpp's statics, not a data.h struct.
const char* ui_get_act_state_str(void);   // "idle"/"working"/"needs_input"/"done"
int ui_get_act_agents(void);
const char* ui_get_daemon_state(void);    // "" = ok/unknown, else last {"src":"status"} state
bool ui_banner_visible(void);                    // main.cpp: a press dismisses it
void ui_hide_banner(void);
bool ui_claude_working(void);                     // main.cpp: backlight breathe
void ui_timer_toggle(void);                        // Clock screen long-press: focus timer
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
void ui_cycle_screen(void);
void ui_toggle_splash(void);
void ui_flash_feedback(void);         // light pulse — short press
void ui_flash_feedback_strong(void);  // firmer pulse — long press / action
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_wifi_status(wifi_state_t state, const char* ip, const char* token);
void ui_update_battery(int percent, bool charging);
