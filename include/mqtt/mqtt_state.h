#pragma once

#include "app_state.h"

#include <stdbool.h>
#include <stddef.h>

#define MAX_PLAYERS 8
#define SCREEN_RTSP_URL_SENSOR "screen_rtsp_url"

typedef struct {
    bool initialized;
    char battery[16];
    char charging[8];
    char charger_type[32];
    char battery_voltage[16];
    char battery_temperature[16];
    char battery_health[16];
    char brightness[16];
    char backlight[16];
    char volume[16];
    char audio_target[24];
    char game_running[8];
    char current_game_id[24];
    char current_game_name[0x200];
    char screen_rtsp_url[128];
    char player_controller_count[16];
    char player_controller[MAX_PLAYERS][32];
    char player_wired[MAX_PLAYERS][8];
    char docked[8];
    char soc_temperature[16];
    char uptime[24];
    char wifi_rssi[16];
    char sd_free[16];
    char sd_total[16];
    char internal_free[16];
    char internal_total[16];
    char memory_used[16];
} SensorSnapshot;

void mqtt_publish_switch_sensors(AppState *state, int fd, const AppConfig *config, SensorSnapshot *snapshot, bool force);
void mqtt_publish_state(AppState *state, int fd, const AppConfig *config, const char *status);
