#include "mqtt/mqtt_commands.h"
#include "mqtt/mqtt_proto.h"
#include "mqtt/mqtt_topics.h"

#include <switch.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define NOTIFICATION_CURRENT_PATH "sdmc:/switch/switch-ha/notification-current.ini"
#define NOTIFICATION_LOG_PATH "sdmc:/switch/switch-ha/notifications.log"
#define NOTIFICATION_LOG_MAX_BYTES 8192

static void trim_text(char *value) {
    while (value[0] == ' ' || value[0] == '\t' || value[0] == '\r' || value[0] == '\n') {
        memmove(value, value + 1, strlen(value));
    }

    size_t len = strlen(value);
    while (len > 0) {
        char ch = value[len - 1];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            break;
        }
        value[--len] = '\0';
    }
}

static void copy_text(char *out, size_t out_size, const char *text) {
    if (out_size == 0) {
        return;
    }
    size_t len = strnlen(text, out_size - 1);
    memcpy(out, text, len);
    out[len] = '\0';
}

static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static bool read_json_hex4(const char *pos, u32 *out) {
    u32 value = 0;
    for (int i = 0; i < 4; ++i) {
        int digit = hex_value(pos[i]);
        if (digit < 0) {
            return false;
        }
        value = (value << 4) | (u32) digit;
    }
    *out = value;
    return true;
}

static void append_utf8(char *out, size_t out_size, size_t *used, u32 cp) {
    if (*used + 1 >= out_size) {
        return;
    }

    if (cp <= 0x7F) {
        out[(*used)++] = (char) cp;
    } else if (cp <= 0x7FF && *used + 2 < out_size) {
        out[(*used)++] = (char) (0xC0 | ((cp >> 6) & 0x1F));
        out[(*used)++] = (char) (0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF && *used + 3 < out_size) {
        out[(*used)++] = (char) (0xE0 | ((cp >> 12) & 0x0F));
        out[(*used)++] = (char) (0x80 | ((cp >> 6) & 0x3F));
        out[(*used)++] = (char) (0x80 | (cp & 0x3F));
    } else if (cp <= 0x10FFFF && *used + 4 < out_size) {
        out[(*used)++] = (char) (0xF0 | ((cp >> 18) & 0x07));
        out[(*used)++] = (char) (0x80 | ((cp >> 12) & 0x3F));
        out[(*used)++] = (char) (0x80 | ((cp >> 6) & 0x3F));
        out[(*used)++] = (char) (0x80 | (cp & 0x3F));
    }
}

static bool json_extract_string(const char *json, const char *key, char *out, size_t out_size) {
    char pattern[40];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *pos = strstr(json, pattern);
    if (!pos) {
        return false;
    }
    pos += strlen(pattern);
    while (*pos == ' ' || *pos == '\t' || *pos == '\r' || *pos == '\n') pos++;
    if (*pos != ':') return false;
    pos++;
    while (*pos == ' ' || *pos == '\t' || *pos == '\r' || *pos == '\n') pos++;
    if (*pos != '"') return false;
    pos++;

    size_t used = 0;
    while (*pos && *pos != '"' && used + 1 < out_size) {
        if (*pos == '\\' && pos[1]) {
            pos++;
            switch (*pos) {
                case 'n': out[used++] = ' '; break;
                case 'r': out[used++] = ' '; break;
                case 't': out[used++] = ' '; break;
                case 'u': {
                    u32 cp = 0;
                    if (read_json_hex4(pos + 1, &cp)) {
                        pos += 5;
                        if (cp >= 0xD800 && cp <= 0xDBFF && pos[0] == '\\' && pos[1] == 'u') {
                            u32 low = 0;
                            if (read_json_hex4(pos + 2, &low) && low >= 0xDC00 && low <= 0xDFFF) {
                                cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                                pos += 6;
                            }
                        }
                        append_utf8(out, out_size, &used, cp);
                    } else {
                        out[used++] = 'u';
                        pos++;
                    }
                    break;
                }
                case '"':
                case '\\':
                case '/':
                    out[used++] = *pos;
                    pos++;
                    break;
                default:
                    out[used++] = *pos;
                    pos++;
                    break;
            }
        } else {
            out[used++] = *pos++;
        }
    }
    out[used] = '\0';
    return used > 0;
}

static void write_sanitized_value(FILE *file, const char *key, const char *value) {
    fprintf(file, "%s=", key);
    for (size_t i = 0; value[i]; ++i) {
        char ch = value[i];
        fputc((ch == '\n' || ch == '\r') ? ' ' : ch, file);
    }
    fputc('\n', file);
}

static void truncate_log_if_needed(const char *path, long max_bytes) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return;
    }
    long size = ftell(file);
    fclose(file);

    if (size > max_bytes) {
        file = fopen(path, "w");
        if (file) {
            fclose(file);
        }
    }
}

static bool write_notification_files(const char *mode, const char *title, const char *message) {
    u64 id = mqtt_monotonic_ms();

    FILE *current = fopen(NOTIFICATION_CURRENT_PATH, "w");
    if (!current) {
        return false;
    }
    fprintf(current, "id=%llu\n", (unsigned long long) id);
    write_sanitized_value(current, "mode", mode);
    write_sanitized_value(current, "title", title);
    write_sanitized_value(current, "message", message);
    fprintf(current, "duration_ms=4500\n");
    fclose(current);

    truncate_log_if_needed(NOTIFICATION_LOG_PATH, NOTIFICATION_LOG_MAX_BYTES);
    FILE *log = fopen(NOTIFICATION_LOG_PATH, "a");
    if (log) {
        fprintf(log, "%llu mode=%s title=%s message=%s\n", (unsigned long long) id, mode, title, message);
        fclose(log);
    }
    return true;
}

#include "title_cache.h"

static void handle_launch_game_payload(AppState *state, int fd, const AppConfig *config, const char *payload) {
    (void) fd;
    (void) config;
    if (!payload || payload[0] == '\0') {
        return;
    }
    app_state_push_log(state, "Game launch request: %s", payload);

    if (strcasecmp(payload, "None") == 0 || strcasecmp(payload, "none") == 0 ||
        strcasecmp(payload, "off") == 0 || strcasecmp(payload, "Home") == 0) {
        app_state_push_log(state, "Game selection None/off requested");
        return;
    }

    u64 app_id = 0;
    if (title_cache_find_application_id_by_name(payload, &app_id)) {
        title_cache_launch_game(state, app_id);
    } else {
        app_state_push_log(state, "Game not found for: %s", payload);
    }
}

static void handle_remote_command(AppState *state, int fd, const AppConfig *config, const char *cmd) {
    (void) fd;
    (void) config;
    if (!cmd || cmd[0] == '\0') {
        return;
    }
    app_state_push_log(state, "Remote command: %s", cmd);

    if (strcasecmp(cmd, "reboot") == 0 || strcasecmp(cmd, "restart") == 0) {
        if (state->svc_spsm_ready) {
            app_state_push_log(state, "Remote reboot accepted");
            spsmShutdown(true);
        }
    } else if (strcasecmp(cmd, "shutdown") == 0 || strcasecmp(cmd, "poweroff") == 0 || strcasecmp(cmd, "turn_off") == 0 || strcasecmp(cmd, "off") == 0) {
        if (state->svc_spsm_ready) {
            app_state_push_log(state, "Remote shutdown accepted");
            spsmShutdown(false);
        }
    } else if (strcasecmp(cmd, "volume_up") == 0) {
        if (state->svc_audctl_ready) {
            AudioTarget audio_target = AudioTarget_Invalid;
            if (R_SUCCEEDED(audctlGetActiveOutputTarget(&audio_target)) || R_SUCCEEDED(audctlGetDefaultTarget(&audio_target))) {
                s32 vol = 0, min_v = 0, max_v = 0;
                if (R_SUCCEEDED(audctlGetTargetVolume(&vol, audio_target)) &&
                    R_SUCCEEDED(audctlGetTargetVolumeMin(&min_v)) &&
                    R_SUCCEEDED(audctlGetTargetVolumeMax(&max_v))) {
                    s32 step = (max_v - min_v) / 20;
                    if (step < 1) step = 1;
                    vol += step;
                    if (vol > max_v) vol = max_v;
                    audctlSetTargetVolume(audio_target, vol);
                    mutexLock(&state->lock);
                    state->sensor_publish_requested = true;
                    mutexUnlock(&state->lock);
                }
            }
        }
    } else if (strcasecmp(cmd, "volume_down") == 0) {
        if (state->svc_audctl_ready) {
            AudioTarget audio_target = AudioTarget_Invalid;
            if (R_SUCCEEDED(audctlGetActiveOutputTarget(&audio_target)) || R_SUCCEEDED(audctlGetDefaultTarget(&audio_target))) {
                s32 vol = 0, min_v = 0, max_v = 0;
                if (R_SUCCEEDED(audctlGetTargetVolume(&vol, audio_target)) &&
                    R_SUCCEEDED(audctlGetTargetVolumeMin(&min_v)) &&
                    R_SUCCEEDED(audctlGetTargetVolumeMax(&max_v))) {
                    s32 step = (max_v - min_v) / 20;
                    if (step < 1) step = 1;
                    vol -= step;
                    if (vol < min_v) vol = min_v;
                    audctlSetTargetVolume(audio_target, vol);
                    mutexLock(&state->lock);
                    state->sensor_publish_requested = true;
                    mutexUnlock(&state->lock);
                }
            }
        }
    } else if (strcasecmp(cmd, "volume_mute") == 0 || strcasecmp(cmd, "mute") == 0) {
        if (state->svc_audctl_ready) {
            AudioTarget audio_target = AudioTarget_Invalid;
            if (R_SUCCEEDED(audctlGetActiveOutputTarget(&audio_target)) || R_SUCCEEDED(audctlGetDefaultTarget(&audio_target))) {
                s32 min_v = 0;
                if (R_SUCCEEDED(audctlGetTargetVolumeMin(&min_v))) {
                    audctlSetTargetVolume(audio_target, min_v);
                    mutexLock(&state->lock);
                    state->sensor_publish_requested = true;
                    mutexUnlock(&state->lock);
                }
            }
        }
    } else if (strcasecmp(cmd, "brightness_up") == 0) {
        if (state->svc_lbl_ready) {
            float b = 0.0f;
            if (R_SUCCEEDED(lblGetCurrentBrightnessSetting(&b))) {
                b += 0.10f;
                if (b > 1.0f) b = 1.0f;
                lblSetCurrentBrightnessSetting(b);
                mutexLock(&state->lock);
                state->sensor_publish_requested = true;
                mutexUnlock(&state->lock);
            }
        }
    } else if (strcasecmp(cmd, "brightness_down") == 0) {
        if (state->svc_lbl_ready) {
            float b = 0.0f;
            if (R_SUCCEEDED(lblGetCurrentBrightnessSetting(&b))) {
                b -= 0.10f;
                if (b < 0.0f) b = 0.0f;
                lblSetCurrentBrightnessSetting(b);
                mutexLock(&state->lock);
                state->sensor_publish_requested = true;
                mutexUnlock(&state->lock);
            }
        }
    } else if (strcasecmp(cmd, "screen_on") == 0) {
        if (state->svc_lbl_ready) {
            lblSwitchBacklightOn(0);
            mutexLock(&state->lock);
            state->sensor_publish_requested = true;
            mutexUnlock(&state->lock);
        }
    } else if (strcasecmp(cmd, "screen_off") == 0) {
        if (state->svc_lbl_ready) {
            lblSwitchBacklightOff(0);
            mutexLock(&state->lock);
            state->sensor_publish_requested = true;
            mutexUnlock(&state->lock);
        }
    } else if (strncasecmp(cmd, "launch_game ", 12) == 0) {
        handle_launch_game_payload(state, fd, config, cmd + 12);
    } else if (strncasecmp(cmd, "launch ", 7) == 0) {
        handle_launch_game_payload(state, fd, config, cmd + 7);
    } else if (strncasecmp(cmd, "select_source ", 14) == 0) {
        handle_launch_game_payload(state, fd, config, cmd + 14);
    } else if (strncasecmp(cmd, "source ", 7) == 0) {
        handle_launch_game_payload(state, fd, config, cmd + 7);
    } else if (strncasecmp(cmd, "channel ", 8) == 0) {
        handle_launch_game_payload(state, fd, config, cmd + 8);
    } else if (strncasecmp(cmd, "activity ", 9) == 0) {
        handle_launch_game_payload(state, fd, config, cmd + 9);
    } else {
        u64 app_id = 0;
        if (title_cache_find_application_id_by_name(cmd, &app_id)) {
            title_cache_launch_game(state, app_id);
        }
    }
}

static void handle_remote_send_command_payload(AppState *state, int fd, const AppConfig *config, const char *payload) {
    if (!payload || payload[0] == '\0') {
        return;
    }

    if (payload[0] == '{') {
        char cmd[128];
        if (json_extract_string(payload, "command", cmd, sizeof(cmd))) {
            handle_remote_command(state, fd, config, cmd);
            return;
        }
        if (json_extract_string(payload, "activity", cmd, sizeof(cmd))) {
            handle_launch_game_payload(state, fd, config, cmd);
            return;
        }
    }

    const char *ptr = payload;
    while (*ptr) {
        while (*ptr == ' ' || *ptr == '\t' || *ptr == '\r' || *ptr == '\n' || *ptr == ',' || *ptr == '"' || *ptr == '[' || *ptr == ']') {
            ptr++;
        }
        if (!*ptr) break;
        const char *start = ptr;
        while (*ptr && *ptr != ' ' && *ptr != '\t' && *ptr != '\r' && *ptr != '\n' && *ptr != ',' && *ptr != '"' && *ptr != ']' && *ptr != '[') {
            ptr++;
        }
        size_t len = (size_t)(ptr - start);
        if (len > 0 && len < 128) {
            char cmd[128];
            memcpy(cmd, start, len);
            cmd[len] = '\0';
            handle_remote_command(state, fd, config, cmd);
        }
    }
}

static void handle_remote_set_payload(AppState *state, int fd, const AppConfig *config, const char *payload) {
    (void) fd;
    (void) config;
    if (!payload || payload[0] == '\0') {
        return;
    }
    app_state_push_log(state, "Remote set: %s", payload);
    if (strcasecmp(payload, "OFF") == 0 || strcasecmp(payload, "false") == 0 || strcasecmp(payload, "0") == 0) {
        if (state->svc_spsm_ready) {
            app_state_push_log(state, "Remote set OFF -> shutdown");
            spsmShutdown(false);
        }
    }
}

static void handle_media_player_cmd(AppState *state, int fd, const AppConfig *config, const char *payload) {
    if (!payload || payload[0] == '\0') {
        return;
    }
    app_state_push_log(state, "Media player cmd: %s", payload);
    if (strcasecmp(payload, "volume_up") == 0 || strcasecmp(payload, "volume_down") == 0 ||
        strcasecmp(payload, "mute") == 0 || strcasecmp(payload, "volume_mute") == 0 ||
        strcasecmp(payload, "turn_off") == 0 || strcasecmp(payload, "off") == 0) {
        handle_remote_command(state, fd, config, payload);
    } else if (strncasecmp(payload, "select_source ", 14) == 0) {
        handle_launch_game_payload(state, fd, config, payload + 14);
    } else if (strncasecmp(payload, "source ", 7) == 0) {
        handle_launch_game_payload(state, fd, config, payload + 7);
    } else if (strncasecmp(payload, "channel ", 8) == 0) {
        handle_launch_game_payload(state, fd, config, payload + 8);
    } else {
        handle_remote_command(state, fd, config, payload);
    }
}

static void handle_media_player_volume_set(AppState *state, int fd, const AppConfig *config, const char *payload) {
    (void) fd;
    (void) config;
    if (!payload || payload[0] == '\0') {
        return;
    }
    double frac = atof(payload);
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    if (state->svc_audctl_ready) {
        AudioTarget audio_target = AudioTarget_Invalid;
        if (R_SUCCEEDED(audctlGetActiveOutputTarget(&audio_target)) || R_SUCCEEDED(audctlGetDefaultTarget(&audio_target))) {
            s32 min_v = 0, max_v = 0;
            if (R_SUCCEEDED(audctlGetTargetVolumeMin(&min_v)) && R_SUCCEEDED(audctlGetTargetVolumeMax(&max_v)) && max_v > min_v) {
                s32 vol = min_v + (s32)(frac * (double)(max_v - min_v));
                if (vol < min_v) vol = min_v;
                if (vol > max_v) vol = max_v;
                audctlSetTargetVolume(audio_target, vol);
                mutexLock(&state->lock);
                state->sensor_publish_requested = true;
                mutexUnlock(&state->lock);
            }
        }
    }
}

static void handle_command_payload(AppState *state, int fd, const AppConfig *config, const char *payload) {
    if (!payload || payload[0] == '\0') {
        return;
    }

    app_state_push_log(state, "MQTT command: %s", payload);

    if (strcasecmp(payload, "reboot") == 0) {
        if (!state->svc_spsm_ready) {
            app_state_push_log(state, "Reboot unavailable: spsm not ready");
            return;
        }
        app_state_push_log(state, "Reboot command accepted");
        spsmShutdown(true);
    } else if (strcasecmp(payload, "shutdown") == 0 || strcasecmp(payload, "poweroff") == 0) {
        if (!state->svc_spsm_ready) {
            app_state_push_log(state, "Shutdown unavailable: spsm not ready");
            return;
        }
        app_state_push_log(state, "Shutdown command accepted");
        spsmShutdown(false);
    } else if (strcasecmp(payload, "sleep") == 0 || strcasecmp(payload, "standby") == 0) {
        app_state_push_log(state, "Sleep command removed: unsupported in sysmodule");
    } else if (strncasecmp(payload, "launch_game ", 12) == 0) {
        handle_launch_game_payload(state, fd, config, payload + 12);
    } else if (strncasecmp(payload, "launch ", 7) == 0) {
        handle_launch_game_payload(state, fd, config, payload + 7);
    } else if (strncasecmp(payload, "select_source ", 14) == 0) {
        handle_launch_game_payload(state, fd, config, payload + 14);
    } else {
        handle_remote_command(state, fd, config, payload);
    }
}

static void handle_notification_payload(AppState *state, int fd, const AppConfig *config, const char *topic, const char *payload) {
    char popup_topic[192];
    char status_topic[192];
    char mode[16];
    char title[96];
    char message[NOTIFY_TEXT_MAX];

    (void) topic;
    mqtt_notification_topic(config, "popup", popup_topic, sizeof(popup_topic));
    mqtt_notification_status_topic(config, status_topic, sizeof(status_topic));

    snprintf(mode, sizeof(mode), "popup");
    snprintf(title, sizeof(title), "Home Assistant");
    copy_text(message, sizeof(message), payload);

    if (payload[0] == '{') {
        json_extract_string(payload, "title", title, sizeof(title));
        json_extract_string(payload, "message", message, sizeof(message));
    } else {
        const char *line = strchr(payload, '\n');
        if (line && line > payload && (size_t) (line - payload) < sizeof(title)) {
            memcpy(title, payload, (size_t) (line - payload));
            title[line - payload] = '\0';
            copy_text(message, sizeof(message), line + 1);
        }
    }

    trim_text(mode);
    trim_text(title);
    trim_text(message);
    if (message[0] == '\0') {
        mqtt_send_publish(fd, status_topic, "empty", false);
        return;
    }

    bool ok = write_notification_files(mode, title, message);
    mqtt_send_publish(fd, status_topic, ok ? "queued" : "queue_failed", false);
    app_state_push_log(state, "Notify %s: %.48s", mode, message);
}

void mqtt_process_overlay_commands(AppState *state, int fd, const AppConfig *config) {
    FILE *file = fopen("sdmc:/switch/switch-ha/overlay-command.ini", "r");
    if (!file) {
        return;
    }

    char action[64] = "toggle";
    char domain[64] = "";
    char service[64] = "";
    char entity_id[128] = "";
    char name[128] = "";
    char line[256];

    while (fgets(line, sizeof(line), file)) {
        trim_text(line);
        char *sep = strchr(line, '=');
        if (!sep) continue;
        *sep = '\0';
        const char *key = line;
        const char *val = sep + 1;

        if (strcasecmp(key, "action") == 0) {
            snprintf(action, sizeof(action), "%s", val);
        } else if (strcasecmp(key, "domain") == 0) {
            snprintf(domain, sizeof(domain), "%s", val);
        } else if (strcasecmp(key, "service") == 0) {
            snprintf(service, sizeof(service), "%s", val);
        } else if (strcasecmp(key, "entity_id") == 0) {
            snprintf(entity_id, sizeof(entity_id), "%s", val);
        } else if (strcasecmp(key, "name") == 0) {
            snprintf(name, sizeof(name), "%s", val);
        }
    }
    fclose(file);
    remove("sdmc:/switch/switch-ha/overlay-command.ini");

    if (domain[0] == '\0' && entity_id[0] != '\0') {
        const char *dot = strchr(entity_id, '.');
        if (dot && (size_t)(dot - entity_id) < sizeof(domain)) {
            memcpy(domain, entity_id, dot - entity_id);
            domain[dot - entity_id] = '\0';
        }
    }
    if (service[0] == '\0') {
        snprintf(service, sizeof(service), "%s", action);
    }

    char event_top[192];
    char service_top[192];
    mqtt_overlay_event_topic(config, event_top, sizeof(event_top));
    mqtt_call_service_topic(config, service_top, sizeof(service_top));

    char payload_event[256];
    snprintf(payload_event, sizeof(payload_event),
             "{\"event_type\":\"%s\",\"entity_id\":\"%s\",\"domain\":\"%s\",\"service\":\"%s\"}",
             action, entity_id, domain, service);
    mqtt_send_publish(fd, event_top, payload_event, false);

    char payload_service[256];
    snprintf(payload_service, sizeof(payload_service),
             "{\"domain\":\"%s\",\"service\":\"%s\",\"entity_id\":\"%s\"}",
             domain[0] ? domain : "homeassistant", service, entity_id);
    mqtt_send_publish(fd, service_top, payload_service, false);

    char toast[128];
    const char *display_target = name[0] ? name : (entity_id[0] ? entity_id : action);
    snprintf(toast, sizeof(toast), "HA Action: %s (%s)", display_target, service);
    write_notification_files("popup", "HA Control", toast);
    app_state_push_log(state, "Overlay command executed: %s", toast);
}

void mqtt_handle_publish(AppState *state, int fd, const AppConfig *config, const unsigned char *buffer, int size) {
    int remaining = 0;
    int remaining_bytes = 0;
    if (!mqtt_read_remaining_length(buffer, size, &remaining, &remaining_bytes)) {
        return;
    }

    int pos = 1 + remaining_bytes;
    if (pos + 2 > size) {
        return;
    }

    int topic_len = ((int) buffer[pos] << 8) | buffer[pos + 1];
    pos += 2;
    if (topic_len <= 0 || pos + topic_len > size) {
        return;
    }

    char topic[192];
    int copy_topic_len = topic_len;
    if (copy_topic_len >= (int) sizeof(topic)) {
        copy_topic_len = (int) sizeof(topic) - 1;
    }
    memcpy(topic, buffer + pos, (size_t) copy_topic_len);
    topic[copy_topic_len] = '\0';
    pos += topic_len;

    int qos = (buffer[0] & 0x06) >> 1;
    if (qos > 0) {
        pos += 2;
    }
    if (pos > size) {
        return;
    }

    int payload_len = size - pos;
    if (payload_len <= 0) {
        return;
    }
    if (payload_len >= NOTIFY_TEXT_MAX) {
        payload_len = NOTIFY_TEXT_MAX - 1;
    }

    char payload[NOTIFY_TEXT_MAX];
    memcpy(payload, buffer + pos, (size_t) payload_len);
    payload[payload_len] = '\0';

    char command_topic[SHA_MAX_TOPIC + 32];
    char popup_topic[192];
    char send_cmd_topic[192];
    char cmd_set_topic[192];
    char mp_cmd_topic[192];
    char mp_vol_cmd_topic[192];
    char mp_src_cmd_topic[192];
    char rem_act_cmd_topic[192];
    char sel_game_cmd_topic[192];

    snprintf(command_topic, sizeof(command_topic), "%s/command", config->mqtt_topic_prefix);
    mqtt_notification_topic(config, "popup", popup_topic, sizeof(popup_topic));
    mqtt_remote_send_command_topic(config, send_cmd_topic, sizeof(send_cmd_topic));
    mqtt_remote_set_topic(config, cmd_set_topic, sizeof(cmd_set_topic));
    mqtt_media_player_command_topic(config, mp_cmd_topic, sizeof(mp_cmd_topic));
    mqtt_media_player_volume_cmd_topic(config, mp_vol_cmd_topic, sizeof(mp_vol_cmd_topic));
    mqtt_media_player_source_cmd_topic(config, mp_src_cmd_topic, sizeof(mp_src_cmd_topic));
    mqtt_remote_activity_cmd_topic(config, rem_act_cmd_topic, sizeof(rem_act_cmd_topic));
    mqtt_select_game_cmd_topic(config, sel_game_cmd_topic, sizeof(sel_game_cmd_topic));

    if (strcmp(topic, command_topic) == 0) {
        handle_command_payload(state, fd, config, payload);
    } else if (strcmp(topic, popup_topic) == 0) {
        handle_notification_payload(state, fd, config, topic, payload);
    } else if (strcmp(topic, send_cmd_topic) == 0) {
        handle_remote_send_command_payload(state, fd, config, payload);
    } else if (strcmp(topic, cmd_set_topic) == 0) {
        handle_remote_set_payload(state, fd, config, payload);
    } else if (strcmp(topic, mp_cmd_topic) == 0) {
        handle_media_player_cmd(state, fd, config, payload);
    } else if (strcmp(topic, mp_vol_cmd_topic) == 0) {
        handle_media_player_volume_set(state, fd, config, payload);
    } else if (strcmp(topic, mp_src_cmd_topic) == 0 ||
               strcmp(topic, rem_act_cmd_topic) == 0 ||
               strcmp(topic, sel_game_cmd_topic) == 0) {
        handle_launch_game_payload(state, fd, config, payload);
    }
}
