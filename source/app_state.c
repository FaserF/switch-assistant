#include "app_state.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "mqtt/mqtt_state.h"

void app_state_init(AppState *state) {
    memset(state, 0, sizeof(*state));
    mutexInit(&state->lock);
    state->app_mode = AppMode_ForegroundApp;
    snprintf(state->status_line, sizeof(state->status_line), "Idle");
    snprintf(state->config_status, sizeof(state->config_status), "not loaded");
    snprintf(state->mqtt_last_error, sizeof(state->mqtt_last_error), "not connected");
    snprintf(state->game_status, sizeof(state->game_status), "not checked");
}

void app_state_refresh_device_info(AppState *state) {
    if (!state) return;

    mutexLock(&state->lock);
    if (state->cached_mac[0] == '\0') {
        if (R_SUCCEEDED(setcalInitialize())) {
            SetCalMacAddress mac = {0};
            if (R_SUCCEEDED(setcalGetWirelessLanMacAddress(&mac))) {
                if (mac.addr[0] != 0 || mac.addr[1] != 0 || mac.addr[2] != 0 ||
                    mac.addr[3] != 0 || mac.addr[4] != 0 || mac.addr[5] != 0) {
                    snprintf(state->cached_mac, sizeof(state->cached_mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                             mac.addr[0], mac.addr[1], mac.addr[2], mac.addr[3], mac.addr[4], mac.addr[5]);
                }
            }
            setcalExit();
        }
    }

    if (state->cached_firmware_ver[0] == '\0' || strcmp(state->cached_firmware_ver, "Unknown") == 0) {
        mqtt_get_system_versions(state, state->cached_firmware_ver, sizeof(state->cached_firmware_ver),
                                 state->cached_atmosphere_ver, sizeof(state->cached_atmosphere_ver));
    }
    mutexUnlock(&state->lock);
}

static void write_line(char *dst, size_t dst_size, const char *fmt, va_list args) {
    vsnprintf(dst, dst_size, fmt, args);
    dst[dst_size - 1] = '\0';
}

void app_state_set_status(AppState *state, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    mutexLock(&state->lock);
    write_line(state->status_line, sizeof(state->status_line), fmt, args);
    mutexUnlock(&state->lock);
    va_end(args);
}

void app_state_push_log(AppState *state, const char *fmt, ...) {
    char line[SHA_LOG_LINE];
    va_list args;
    va_start(args, fmt);
    write_line(line, sizeof(line), fmt, args);
    va_end(args);

    mutexLock(&state->lock);
    for (int i = SHA_MAX_LOG - 1; i > 0; --i) {
        strncpy(state->log_lines[i], state->log_lines[i - 1], SHA_LOG_LINE - 1);
        state->log_lines[i][SHA_LOG_LINE - 1] = '\0';
    }
    strncpy(state->log_lines[0], line, SHA_LOG_LINE - 1);
    state->log_lines[0][SHA_LOG_LINE - 1] = '\0';
    if (state->log_count < SHA_MAX_LOG) {
        state->log_count++;
    }
    mutexUnlock(&state->lock);
}
