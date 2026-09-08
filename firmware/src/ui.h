#pragma once
#include "data.h"
#include "ble.h"

enum screen_t {
    SCREEN_SPLASH,
    SCREEN_CLOCK,
    SCREEN_SENSOR,
    SCREEN_USAGE,
    SCREEN_COPILOT,
    SCREEN_SYSINFO,
    SCREEN_VSCODE,
    SCREEN_BLUETOOTH,
    SCREEN_CI,
    SCREEN_TODAY,
    SCREEN_COUNT,
};

void ui_init(void);
void ui_update(const UsageData* data);
void ui_update_copilot(const CopilotData* data);
void ui_update_sysinfo(const SysInfoData* data);
void ui_update_vscode(const VscodeData* data);
void ui_update_env(const EnvData* data);         // {"src":"env",...} — clock + weather
void ui_update_sensor(bool present, float temp_c, float pressure_hpa,
                       bool has_humidity, float humidity_pct); // env sensor (BME280/BMP180), on-device
void ui_update_ci(const CiData* data);           // {"src":"ci",...}  — CI + review queue
void ui_update_today(int act_min, int tok_k, int usd, int commits, int cp_used); // {"src":"sum",...}
void ui_update_act(const char* state, int agents); // {"src":"act",...} — Claude activity
void ui_update_daemon_state(const char* state);  // {"src":"status","state":...}
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
void ui_update_battery(int percent, bool charging);
