#include "mqtt/mqtt_discovery.h"
#include "mqtt/mqtt_proto.h"
#include "mqtt/mqtt_state.h"
#include "mqtt/mqtt_topics.h"

#include <stdio.h>
#include <string.h>

static void publish_discovery_sensor_ext(int fd, const AppConfig *config, const char *component, const char *sensor,
                                         const char *name, const char *device_class, const char *unit, const char *state_class,
                                         const char *entity_category, const char *icon, bool enabled_by_default) {
    char topic[256];
    char state[192];
    char payload[MQTT_BUFFER];
    char safe_id[80];

    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    mqtt_discovery_topic(config, component, sensor, topic, sizeof(topic));
    mqtt_state_topic(config, sensor, state, sizeof(state));

    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"stat_t\":\"%s\",\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"Nintendo\",\"mdl\":\"Switch\"}%s%s%s%s}",
             name,
             safe_id,
             sensor,
             state,
             safe_id,
             config->device_name,
             device_class ? ",\"dev_cla\":\"" : "",
             device_class ? device_class : "",
             device_class ? "\"" : "",
             !enabled_by_default ? ",\"enabled_by_default\":false" : "");

    if (unit) {
        size_t len = strlen(payload);
        if (len > 0 && payload[len - 1] == '}') {
            payload[len - 1] = '\0';
            snprintf(payload + strlen(payload), sizeof(payload) - strlen(payload), ",\"unit_of_meas\":\"%s\"}", unit);
        }
    }
    if (state_class) {
        size_t len = strlen(payload);
        if (len > 0 && payload[len - 1] == '}') {
            payload[len - 1] = '\0';
            snprintf(payload + strlen(payload), sizeof(payload) - strlen(payload), ",\"stat_cla\":\"%s\"}", state_class);
        }
    }
    if (entity_category) {
        size_t len = strlen(payload);
        if (len > 0 && payload[len - 1] == '}') {
            payload[len - 1] = '\0';
            snprintf(payload + strlen(payload), sizeof(payload) - strlen(payload), ",\"ent_cat\":\"%s\"}", entity_category);
        }
    }
    if (icon) {
        size_t len = strlen(payload);
        if (len > 0 && payload[len - 1] == '}') {
            payload[len - 1] = '\0';
            snprintf(payload + strlen(payload), sizeof(payload) - strlen(payload), ",\"ic\":\"%s\"}", icon);
        }
    }

    mqtt_send_publish(fd, topic, payload, true);
}

static void publish_discovery_sensor(int fd, const AppConfig *config, const char *component, const char *sensor,
                                     const char *name, const char *device_class, const char *unit, const char *state_class) {
    publish_discovery_sensor_ext(fd, config, component, sensor, name, device_class, unit, state_class, NULL, NULL, true);
}

static void publish_discovery_sensor_disabled(int fd, const AppConfig *config, const char *component, const char *sensor,
                                              const char *name, const char *device_class, const char *unit, const char *state_class,
                                              const char *entity_category, const char *icon) {
    publish_discovery_sensor_ext(fd, config, component, sensor, name, device_class, unit, state_class, entity_category, icon, false);
}

static void publish_discovery_button(int fd, const AppConfig *config, const char *button, const char *name, const char *payload_press) {
    char topic[256];
    char command_topic[SHA_MAX_TOPIC + 32];
    char payload[MQTT_BUFFER];
    char safe_id[80];

    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    mqtt_discovery_topic(config, "button", button, topic, sizeof(topic));
    snprintf(command_topic, sizeof(command_topic), "%s/command", config->mqtt_topic_prefix);

    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"cmd_t\":\"%s\",\"payload_press\":\"%s\",\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"Nintendo\",\"mdl\":\"Switch\"}}",
             name,
             safe_id,
             button,
             command_topic,
             payload_press,
             safe_id,
             config->device_name);

    mqtt_send_publish(fd, topic, payload, true);
}

static void publish_discovery_notify(int fd, const AppConfig *config, const char *mode, const char *name) {
    char topic[256];
    char command_topic[192];
    char payload[MQTT_BUFFER];
    char safe_id[80];

    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    mqtt_discovery_topic(config, "notify", mode, topic, sizeof(topic));
    mqtt_notification_topic(config, mode, command_topic, sizeof(command_topic));

    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"uniq_id\":\"%s_notify_%s\",\"cmd_t\":\"%s\","
             "\"cmd_tpl\":\"{{ value }}\","
             "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"Nintendo\",\"mdl\":\"Switch\"}}",
             name,
             safe_id,
             mode,
             command_topic,
             safe_id,
             config->device_name);

    mqtt_send_publish(fd, topic, payload, true);
}

#include "title_cache.h"
#include <stdlib.h>

static void publish_discovery_remote(int fd, const AppConfig *config) {
    char topic[256];
    char send_cmd_topic[192];
    char cmd_set_topic[192];
    char state_top[192];
    char act_cmd_top[192];
    char act_stat_top[192];
    char safe_id[80];

    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    mqtt_discovery_topic(config, "remote", "remote", topic, sizeof(topic));
    mqtt_remote_send_command_topic(config, send_cmd_topic, sizeof(send_cmd_topic));
    mqtt_remote_set_topic(config, cmd_set_topic, sizeof(cmd_set_topic));
    mqtt_remote_state_topic(config, state_top, sizeof(state_top));
    mqtt_remote_activity_cmd_topic(config, act_cmd_top, sizeof(act_cmd_top));
    mqtt_remote_activity_state_topic(config, act_stat_top, sizeof(act_stat_top));

    char options_json[16384];
    title_cache_build_json_options_array(options_json, sizeof(options_json));

    size_t payload_size = sizeof(options_json) + 2048;
    char *payload = malloc(payload_size);
    if (!payload) {
        return;
    }

    snprintf(payload, payload_size,
             "{\"name\":\"Remote\",\"uniq_id\":\"%s_remote\",\"send_cmd_t\":\"%s\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
             "\"activity_cmd_t\":\"%s\",\"activity_stat_t\":\"%s\","
             "\"activity_list\":%s,"
             "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"Nintendo\",\"mdl\":\"Switch\"}}",
             safe_id,
             send_cmd_topic,
             cmd_set_topic,
             state_top,
             act_cmd_top,
             act_stat_top,
             options_json,
             safe_id,
             config->device_name);

    mqtt_send_publish(fd, topic, payload, true);
    free(payload);
}

static void publish_discovery_media_player(int fd, const AppConfig *config) {
    char topic[256];
    char cmd_topic[192];
    char stat_topic[192];
    char vol_cmd_top[192];
    char vol_stat_top[192];
    char src_cmd_top[192];
    char src_stat_top[192];
    char safe_id[80];

    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    mqtt_discovery_topic(config, "media_player", "media_player", topic, sizeof(topic));
    mqtt_media_player_command_topic(config, cmd_topic, sizeof(cmd_topic));
    mqtt_media_player_state_topic(config, stat_topic, sizeof(stat_topic));
    mqtt_media_player_volume_cmd_topic(config, vol_cmd_top, sizeof(vol_cmd_top));
    mqtt_media_player_volume_state_topic(config, vol_stat_top, sizeof(vol_stat_top));
    mqtt_media_player_source_cmd_topic(config, src_cmd_top, sizeof(src_cmd_top));
    mqtt_media_player_source_state_topic(config, src_stat_top, sizeof(src_stat_top));

    char options_json[16384];
    title_cache_build_json_options_array(options_json, sizeof(options_json));

    size_t payload_size = sizeof(options_json) + 2048;
    char *payload = malloc(payload_size);
    if (!payload) {
        return;
    }

    snprintf(payload, payload_size,
             "{\"name\":\"Media Player\",\"uniq_id\":\"%s_media_player\","
             "\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
             "\"vol_cmd_t\":\"%s\",\"vol_stat_t\":\"%s\","
             "\"source_cmd_t\":\"%s\",\"source_stat_t\":\"%s\","
             "\"source_list\":%s,"
             "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"Nintendo\",\"mdl\":\"Switch\"}}",
             safe_id,
             cmd_topic,
             stat_topic,
             vol_cmd_top,
             vol_stat_top,
             src_cmd_top,
             src_stat_top,
             options_json,
             safe_id,
             config->device_name);

    mqtt_send_publish(fd, topic, payload, true);
    free(payload);
}

static void publish_discovery_select_game(int fd, const AppConfig *config) {
    char topic[256];
    char cmd_topic[192];
    char stat_topic[192];
    char safe_id[80];

    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    mqtt_discovery_topic(config, "select", "game", topic, sizeof(topic));
    mqtt_select_game_cmd_topic(config, cmd_topic, sizeof(cmd_topic));
    mqtt_select_game_state_topic(config, stat_topic, sizeof(stat_topic));

    char options_json[16384];
    title_cache_build_json_options_array(options_json, sizeof(options_json));

    size_t payload_size = sizeof(options_json) + 2048;
    char *payload = malloc(payload_size);
    if (!payload) {
        return;
    }

    snprintf(payload, payload_size,
             "{\"name\":\"Channel\",\"uniq_id\":\"%s_select_game\","
             "\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
             "\"options\":%s,"
             "\"ic\":\"mdi:television-box\","
             "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"Nintendo\",\"mdl\":\"Switch\"}}",
             safe_id,
             cmd_topic,
             stat_topic,
             options_json,
             safe_id,
             config->device_name);

    mqtt_send_publish(fd, topic, payload, true);
    free(payload);
}

static void publish_discovery_select_game_players(int fd, const AppConfig *config, const char *sensor, const char *name, int min_players, const char *icon) {
    char topic[256];
    char cmd_topic[192];
    char stat_topic[192];
    char safe_id[80];

    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    mqtt_discovery_topic(config, "select", sensor, topic, sizeof(topic));

    if (min_players == 2) {
        mqtt_select_game_2p_cmd_topic(config, cmd_topic, sizeof(cmd_topic));
        mqtt_select_game_2p_state_topic(config, stat_topic, sizeof(stat_topic));
    } else if (min_players == 4) {
        mqtt_select_game_4p_cmd_topic(config, cmd_topic, sizeof(cmd_topic));
        mqtt_select_game_4p_state_topic(config, stat_topic, sizeof(stat_topic));
    } else {
        mqtt_select_game_8p_cmd_topic(config, cmd_topic, sizeof(cmd_topic));
        mqtt_select_game_8p_state_topic(config, stat_topic, sizeof(stat_topic));
    }

    char options_json[16384];
    title_cache_build_json_options_array_for_players(options_json, sizeof(options_json), min_players);

    size_t payload_size = sizeof(options_json) + 2048;
    char *payload = malloc(payload_size);
    if (!payload) {
        return;
    }

    snprintf(payload, payload_size,
             "{\"name\":\"%s\",\"uniq_id\":\"%s_select_%s\","
             "\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
             "\"options\":%s,"
             "\"ic\":\"%s\","
             "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"Nintendo\",\"mdl\":\"Switch\"}}",
             name,
             safe_id,
             sensor,
             cmd_topic,
             stat_topic,
             options_json,
             icon,
             safe_id,
             config->device_name);

    mqtt_send_publish(fd, topic, payload, true);
    free(payload);
}

static void publish_discovery_sensor_games_count(int fd, const AppConfig *config, const char *sensor, const char *name, const char *icon) {
    char topic[256];
    char state[192];
    char attr_topic[192];
    char payload[MQTT_BUFFER];
    char safe_id[80];

    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    mqtt_discovery_topic(config, "sensor", sensor, topic, sizeof(topic));
    mqtt_state_topic(config, sensor, state, sizeof(state));
    mqtt_sensor_attr_topic(config, sensor, attr_topic, sizeof(attr_topic));

    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"stat_t\":\"%s\",\"json_attr_t\":\"%s\",\"ic\":\"%s\",\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"Nintendo\",\"mdl\":\"Switch\"}}",
             name,
             safe_id,
             sensor,
             state,
             attr_topic,
             icon,
             safe_id,
             config->device_name);

    mqtt_send_publish(fd, topic, payload, true);
}

static void publish_discovery_update(int fd, const AppConfig *config) {
    char topic[256];
    char state_top[192];
    char cmd_top[192];
    char payload[MQTT_BUFFER];
    char safe_id[80];

    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    mqtt_discovery_topic(config, "update", "firmware", topic, sizeof(topic));
    mqtt_update_state_topic(config, state_top, sizeof(state_top));
    mqtt_update_command_topic(config, cmd_top, sizeof(cmd_top));

    snprintf(payload, sizeof(payload),
             "{\"name\":\"Firmware\",\"uniq_id\":\"%s_firmware\",\"stat_t\":\"%s\",\"cmd_t\":\"%s\","
             "\"title\":\"Nintendo Switch Firmware\",\"dev_cla\":\"firmware\","
             "\"release_url\":\"https://github.com/THZoria/NX_Firmware/releases\","
             "\"entity_picture\":\"https://raw.githubusercontent.com/FaserF/switch-assistant/main/assets/icon.jpg\","
             "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"Nintendo\",\"mdl\":\"Switch\"}}",
             safe_id,
             state_top,
             cmd_top,
             safe_id,
             config->device_name);

    mqtt_send_publish(fd, topic, payload, true);
}

static void publish_discovery_atmosphere_update(int fd, const AppConfig *config) {
    char topic[256];
    char state_top[192];
    char cmd_top[192];
    char payload[MQTT_BUFFER];
    char safe_id[80];

    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    mqtt_discovery_topic(config, "update", "atmosphere", topic, sizeof(topic));
    mqtt_atmosphere_update_state_topic(config, state_top, sizeof(state_top));
    mqtt_atmosphere_update_command_topic(config, cmd_top, sizeof(cmd_top));

    snprintf(payload, sizeof(payload),
             "{\"name\":\"Atmosphère\",\"uniq_id\":\"%s_atmosphere\","
             "\"stat_t\":\"%s\",\"cmd_t\":\"%s\","
             "\"title\":\"Atmosphère Custom Firmware\",\"dev_cla\":\"firmware\","
             "\"release_url\":\"https://github.com/Atmosphere-NX/Atmosphere/releases/latest\","
             "\"entity_picture\":\"https://avatars.githubusercontent.com/u/39328464\","
             "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"Nintendo\",\"mdl\":\"Switch\"}}",
             safe_id,
             state_top,
             cmd_top,
             safe_id,
             config->device_name);

    mqtt_send_publish(fd, topic, payload, true);
}

static void publish_discovery_event(int fd, const AppConfig *config) {
    char topic[256];
    char state_top[192];
    char payload[MQTT_BUFFER];
    char safe_id[80];

    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    mqtt_discovery_topic(config, "event", "overlay_action", topic, sizeof(topic));
    mqtt_overlay_event_topic(config, state_top, sizeof(state_top));

    snprintf(payload, sizeof(payload),
             "{\"name\":\"Overlay Action\",\"uniq_id\":\"%s_overlay_action\","
             "\"stat_t\":\"%s\",\"event_types\":[\"toggle\",\"turn_on\",\"turn_off\",\"press\",\"call_service\",\"quick_action_1\",\"quick_action_2\",\"quick_action_3\",\"quick_action_4\"],"
             "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"Nintendo\",\"mdl\":\"Switch\"}}",
             safe_id,
             state_top,
             safe_id,
             config->device_name);

    mqtt_send_publish(fd, topic, payload, true);
}

void mqtt_clear_discovery_topic(int fd, const AppConfig *config, const char *component, const char *sensor) {
    char topic[256];
    mqtt_discovery_topic(config, component, sensor, topic, sizeof(topic));
    mqtt_send_publish(fd, topic, "", true);
}

void mqtt_publish_sensor_discovery(AppState *state, int fd, const AppConfig *config) {
    // Home Assistant Battery Integration (Core battery % and charging stay enabled)
    publish_discovery_sensor_ext(fd, config, "sensor", "battery", "Battery", "battery", "%", "measurement", NULL, NULL, true);
    publish_discovery_sensor_ext(fd, config, "binary_sensor", "charging", "Battery Charging", "battery_charging", NULL, NULL, NULL, NULL, true);

    // Advanced battery telemetry -> disabled by default
    publish_discovery_sensor_disabled(fd, config, "sensor", "charger_type", "Charger Type", NULL, NULL, NULL, "diagnostic", "mdi:power-plug");
    publish_discovery_sensor_disabled(fd, config, "sensor", "battery_voltage", "Battery Voltage", "voltage", "mV", "measurement", "diagnostic", "mdi:sine-wave");
    publish_discovery_sensor_disabled(fd, config, "sensor", "battery_temperature", "Battery Temperature", "temperature", "°C", "measurement", "diagnostic", "mdi:thermometer");
    publish_discovery_sensor_disabled(fd, config, "sensor", "battery_health", "Battery Health", NULL, "%", "measurement", "diagnostic", "mdi:battery-heart-variant");

    if (state->svc_lbl_ready) {
        publish_discovery_sensor_disabled(fd, config, "sensor", "brightness", "Screen Brightness", NULL, "%", "measurement", "diagnostic", "mdi:brightness-6");
        publish_discovery_sensor_disabled(fd, config, "binary_sensor", "backlight", "Screen", NULL, NULL, NULL, "diagnostic", "mdi:television-ambient-light");
    }
    if (state->svc_audctl_ready) {
        publish_discovery_sensor_disabled(fd, config, "sensor", "volume", "Volume", NULL, "%", "measurement", "diagnostic", "mdi:volume-high");
        publish_discovery_sensor_disabled(fd, config, "sensor", "audio_target", "Audio Output Target", NULL, NULL, NULL, "diagnostic", "mdi:speaker");
    }
    publish_discovery_sensor(fd, config, "binary_sensor", "game_running", "Game Running", "running", NULL, NULL);
    publish_discovery_sensor(fd, config, "sensor", "current_game", "Current Game", NULL, NULL, NULL);
    publish_discovery_sensor_disabled(fd, config, "sensor", "current_game_id", "Current Game Title ID", NULL, NULL, NULL, "diagnostic", "mdi:identifier");
    mqtt_clear_discovery_topic(fd, config, "sensor", "current_game_name");
    mqtt_clear_discovery_topic(fd, config, "sensor", "game_pid");
    if (state->svc_hid_ready) {
        publish_discovery_sensor(fd, config, "sensor", "controller_count", "Player Count", NULL, NULL, "measurement");
        for (int i = 0; i < MAX_PLAYERS; ++i) {
            char sensor[32];
            char name[32];
            snprintf(sensor, sizeof(sensor), "player_%d_controller", i + 1);
            snprintf(name, sizeof(name), "Player %d Controller", i + 1);
            publish_discovery_sensor_disabled(fd, config, "sensor", sensor, name, NULL, NULL, NULL, "diagnostic", "mdi:controller");
        }
    }
    if (state->svc_spsm_ready) {
        publish_discovery_button(fd, config, "reboot", "Reboot", "reboot");
        publish_discovery_button(fd, config, "shutdown", "Shutdown", "shutdown");
    }
    publish_discovery_button(fd, config, "launch_daybreak", "Launch Daybreak", "launch_daybreak");
    title_cache_refresh_installed_games();
    publish_discovery_remote(fd, config);
    publish_discovery_media_player(fd, config);
    publish_discovery_select_game(fd, config);
    publish_discovery_select_game_players(fd, config, "game_2p", "2-Player Games", 2, "mdi:account-multiple");
    publish_discovery_select_game_players(fd, config, "game_4p", "4-Player Games", 4, "mdi:account-group");
    publish_discovery_select_game_players(fd, config, "game_8p", "8+ Player Games", 8, "mdi:account-group-outline");
    publish_discovery_sensor_games_count(fd, config, "games_2p", "2-Player Games", "mdi:account-multiple");
    publish_discovery_sensor_games_count(fd, config, "games_4p", "4-Player Games", "mdi:account-group");
    publish_discovery_sensor_games_count(fd, config, "games_8p", "8+ Player Games", "mdi:account-group-outline");
    publish_discovery_update(fd, config);
    publish_discovery_atmosphere_update(fd, config);
    publish_discovery_event(fd, config);
    publish_discovery_sensor(fd, config, "binary_sensor", "docked", "Docked", "plug", NULL, NULL);
    if (state->svc_ts_ready) {
        publish_discovery_sensor_disabled(fd, config, "sensor", "soc_temperature", "SoC Temperature", "temperature", "°C", "measurement", "diagnostic", "mdi:thermometer-lines");
    }
    publish_discovery_sensor_disabled(fd, config, "sensor", "uptime", "Uptime", "duration", "s", "total_increasing", "diagnostic", "mdi:clock-outline");
    if (state->svc_nifm_ready) {
        publish_discovery_sensor_disabled(fd, config, "sensor", "wifi_rssi", "WiFi Signal Strength", "signal_strength", "dBm", "measurement", "diagnostic", "mdi:wifi");
    }
    publish_discovery_sensor_disabled(fd, config, "sensor", "sd_free", "SD Card Free Space", "data_size", "MB", "measurement", "diagnostic", "mdi:micro-sd");
    publish_discovery_sensor_disabled(fd, config, "sensor", "memory_used", "RAM Used", "data_size", "MB", "measurement", "diagnostic", "mdi:memory");
    if (state->app_mode == AppMode_Sysmodule) {
        publish_discovery_sensor_disabled(fd, config, "sensor", SCREEN_RTSP_URL_SENSOR, "Screen Stream URL", NULL, NULL, NULL, "diagnostic", "mdi:video");
        mqtt_clear_discovery_topic(fd, config, "sensor", "screen_stream_status");
        mqtt_clear_discovery_topic(fd, config, "sensor", "screen_rtsp_port");
        mqtt_clear_discovery_topic(fd, config, "camera", "screen_camera");
    }
    if (config->notification_overlay_enabled) {
        publish_discovery_notify(fd, config, "popup", "Popup Notification");
    } else {
        mqtt_clear_discovery_topic(fd, config, "notify", "popup");
    }
    mqtt_clear_discovery_topic(fd, config, "notify", "modal");
}
