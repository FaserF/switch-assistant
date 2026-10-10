#include "mqtt_client.h"
#include "mqtt/mqtt_proto.h"
#include "mqtt/mqtt_topics.h"
#include "mqtt/mqtt_discovery.h"
#include "mqtt/mqtt_state.h"
#include "mqtt/mqtt_commands.h"

#include <switch.h>

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    Thread thread;
    bool active;
    int socket_fd;
} MqttContext;

static MqttContext g_ctx = {0};

static void mqtt_loop(void *arg) {
    AppState *state = arg;
    u64 last_sensor_poll_ms = 0;
    u64 last_heartbeat_ms = 0;
    app_state_push_log(state, "MQTT thread started");

    while (true) {
        mutexLock(&state->lock);
        bool should_exit = state->exit_requested;
        bool power_sleeping = state->power_sleeping;
        bool dns_ok = state->mqtt_dns_ok;
        AppConfig config = state->config;
        char prefix[SHA_MAX_TOPIC];
        strncpy(prefix, state->config.mqtt_topic_prefix, sizeof(prefix) - 1);
        prefix[sizeof(prefix) - 1] = '\0';
        mutexUnlock(&state->lock);

        char host[SHA_MAX_FIELD];
        int port = config.mqtt_port;
        mqtt_normalize_endpoint(&config, host, sizeof(host), &port);

        if (should_exit) {
            break;
        }

        if (power_sleeping) {
            mqtt_set_status(state, dns_ok, false, false, "sleeping");
            svcSleepThread(1000 * 1000 * 1000ULL);
            continue;
        }

        if (host[0] == '\0' || prefix[0] == '\0') {
            mqtt_set_status(state, false, false, false, "MQTT incomplete config");
            svcSleepThread(1000 * 1000 * 1000ULL);
            continue;
        }

        if (port <= 0 || port > 65535) {
            mqtt_set_status(state, false, false, false, "Invalid MQTT port: %d", port);
            app_state_push_log(state, "MQTT invalid port: %d", port);
            svcSleepThread(2 * 1000 * 1000 * 1000ULL);
            continue;
        }

        if (port == 8123) {
            mqtt_set_status(state, true, false, false, "Port 8123 is HA HTTP; MQTT is usually 1883");
            app_state_push_log(state, "MQTT wrong port: 8123 is HA HTTP");
            svcSleepThread(2 * 1000 * 1000 * 1000ULL);
            continue;
        }

        int fd = -1;
        app_state_push_log(state, "MQTT connecting %s:%d", host, port);
        if (!mqtt_socket_connect_to(state, host, port, &fd)) {
            mutexLock(&state->lock);
            char error[SHA_LOG_LINE];
            strncpy(error, state->mqtt_last_error, sizeof(error) - 1);
            error[sizeof(error) - 1] = '\0';
            mutexUnlock(&state->lock);
            app_state_push_log(state, "MQTT %s", error);
            app_state_set_status(state, "%s", error);
            svcSleepThread(2 * 1000 * 1000 * 1000ULL);
            continue;
        }

        g_ctx.socket_fd = fd;

        if (!mqtt_send_connect(&config, fd)) {
            mqtt_set_status(state, true, true, false, "MQTT CONNECT send failed errno=%d", errno);
            app_state_push_log(state, "MQTT CONNECT send failed errno=%d", errno);
            close(fd);
            g_ctx.socket_fd = -1;
            svcSleepThread(1000 * 1000 * 1000ULL);
            continue;
        }

        unsigned char ack[4] = {0};
        if (!mqtt_socket_recv_all(fd, ack, sizeof(ack)) || ack[0] != 0x20 || ack[1] != 0x02 || ack[3] != 0x00) {
            int return_code = ack[3];
            mqtt_set_status(state, true, true, false, "MQTT rejected rc=%d %s", return_code, mqtt_connack_reason(return_code));
            app_state_push_log(state, "MQTT rejected rc=%d %s", return_code, mqtt_connack_reason(return_code));
            close(fd);
            g_ctx.socket_fd = -1;
            svcSleepThread(2 * 1000 * 1000 * 1000ULL);
            continue;
        }

        char command_topic[SHA_MAX_TOPIC + 32];
        char send_cmd_topic[192];
        char cmd_set_topic[192];
        char mp_cmd_topic[192];
        char mp_vol_cmd_topic[192];
        char mp_src_cmd_topic[192];
        char rem_act_cmd_topic[192];
        char sel_game_cmd_topic[192];
        char sel_game_2p_cmd_topic[192];
        char sel_game_4p_cmd_topic[192];
        char sel_game_8p_cmd_topic[192];
        char update_cmd_topic[192];
        char ams_update_cmd_topic[192];
        snprintf(command_topic, sizeof(command_topic), "%s/command", prefix);
        mqtt_remote_send_command_topic(&config, send_cmd_topic, sizeof(send_cmd_topic));
        mqtt_remote_set_topic(&config, cmd_set_topic, sizeof(cmd_set_topic));
        mqtt_media_player_command_topic(&config, mp_cmd_topic, sizeof(mp_cmd_topic));
        mqtt_media_player_volume_cmd_topic(&config, mp_vol_cmd_topic, sizeof(mp_vol_cmd_topic));
        mqtt_media_player_source_cmd_topic(&config, mp_src_cmd_topic, sizeof(mp_src_cmd_topic));
        mqtt_remote_activity_cmd_topic(&config, rem_act_cmd_topic, sizeof(rem_act_cmd_topic));
        mqtt_select_game_cmd_topic(&config, sel_game_cmd_topic, sizeof(sel_game_cmd_topic));
        mqtt_select_game_2p_cmd_topic(&config, sel_game_2p_cmd_topic, sizeof(sel_game_2p_cmd_topic));
        mqtt_select_game_4p_cmd_topic(&config, sel_game_4p_cmd_topic, sizeof(sel_game_4p_cmd_topic));
        mqtt_select_game_8p_cmd_topic(&config, sel_game_8p_cmd_topic, sizeof(sel_game_8p_cmd_topic));
        mqtt_update_command_topic(&config, update_cmd_topic, sizeof(update_cmd_topic));
        mqtt_atmosphere_update_command_topic(&config, ams_update_cmd_topic, sizeof(ams_update_cmd_topic));
        mqtt_send_subscribe(fd, command_topic);
        mqtt_send_subscribe(fd, send_cmd_topic);
        mqtt_send_subscribe(fd, cmd_set_topic);
        mqtt_send_subscribe(fd, mp_cmd_topic);
        mqtt_send_subscribe(fd, mp_vol_cmd_topic);
        mqtt_send_subscribe(fd, mp_src_cmd_topic);
        mqtt_send_subscribe(fd, rem_act_cmd_topic);
        mqtt_send_subscribe(fd, sel_game_cmd_topic);
        mqtt_send_subscribe(fd, sel_game_2p_cmd_topic);
        mqtt_send_subscribe(fd, sel_game_4p_cmd_topic);
        mqtt_send_subscribe(fd, sel_game_8p_cmd_topic);
        mqtt_send_subscribe(fd, update_cmd_topic);
        mqtt_send_subscribe(fd, ams_update_cmd_topic);
        if (config.notification_overlay_enabled) {
            char popup_topic[192];
            mqtt_notification_topic(&config, "popup", popup_topic, sizeof(popup_topic));
            mqtt_send_subscribe(fd, popup_topic);
        }

        mutexLock(&state->lock);
        state->mqtt_dns_ok = true;
        state->mqtt_tcp_ok = true;
        state->mqtt_connected = true;
        snprintf(state->mqtt_last_error, sizeof(state->mqtt_last_error), "connected");
        mutexUnlock(&state->lock);
        app_state_push_log(state, "MQTT connected");
        app_state_set_status(state, "MQTT connected");
        SensorSnapshot sensor_snapshot = {0};
        mqtt_publish_sensor_discovery(state, fd, &config);
        mqtt_publish_state(state, fd, &config, "online");
        mqtt_publish_switch_sensors(state, fd, &config, &sensor_snapshot, true);
        last_sensor_poll_ms = mqtt_monotonic_ms();
        last_heartbeat_ms = last_sensor_poll_ms;

        while (true) {
            struct pollfd pfd = {.fd = fd, .events = POLLIN};
            int poll_rc = poll(&pfd, 1, 100);
            if (poll_rc < 0) {
                break;
            }
            if (poll_rc > 0 && (pfd.revents & POLLIN)) {
                unsigned char buffer[MQTT_BUFFER];
                int size = recv(fd, buffer, sizeof(buffer), 0);
                if (size <= 0) {
                    break;
                }
                if ((buffer[0] & 0xF0) == 0x30) {
                    mqtt_handle_publish(state, fd, &config, buffer, size);
                }
            }

            mutexLock(&state->lock);
            should_exit = state->exit_requested;
            power_sleeping = state->power_sleeping;
            bool sensor_publish_requested = state->sensor_publish_requested;
            if (sensor_publish_requested) {
                state->sensor_publish_requested = false;
            }
            mutexUnlock(&state->lock);
            if (should_exit) {
                break;
            }

            u64 now = mqtt_monotonic_ms();
            mqtt_process_overlay_commands(state, fd, &config);
            if (sensor_publish_requested || now - last_sensor_poll_ms >= SENSOR_POLL_INTERVAL_MS) {
                mqtt_publish_switch_sensors(state, fd, &config, &sensor_snapshot, false);
                last_sensor_poll_ms = now;
            }

            if (power_sleeping) {
                break;
            }

            if (now - last_heartbeat_ms >= MQTT_HEARTBEAT_INTERVAL_MS) {
                if (!mqtt_send_ping(fd)) {
                    break;
                }
                mqtt_publish_state(state, fd, &config, "online");
                last_heartbeat_ms = now;
            }
        }

        if (!power_sleeping) {
            mqtt_publish_state(state, fd, &config, "offline");
        }
        close(fd);
        g_ctx.socket_fd = -1;
        mutexLock(&state->lock);
        state->mqtt_tcp_ok = false;
        state->mqtt_connected = false;
        snprintf(state->mqtt_last_error, sizeof(state->mqtt_last_error), "%s", power_sleeping ? "sleeping" : "disconnected");
        mutexUnlock(&state->lock);
        if (!power_sleeping) {
            app_state_push_log(state, "MQTT disconnected");
        }
    }
}

bool mqtt_service_start(AppState *state) {
    mutexLock(&state->lock);
    state->mqtt_running = true;
    mutexUnlock(&state->lock);
    g_ctx.socket_fd = -1;
    g_ctx.active = R_SUCCEEDED(threadCreate(&g_ctx.thread, mqtt_loop, state, NULL, 0x8000, 0x2B, -2));
    if (!g_ctx.active) {
        app_state_push_log(state, "Failed to start MQTT thread");
        return false;
    }
    threadStart(&g_ctx.thread);
    return true;
}

void mqtt_service_stop(AppState *state) {
    mutexLock(&state->lock);
    state->exit_requested = true;
    mutexUnlock(&state->lock);

    if (g_ctx.socket_fd >= 0) {
        shutdown(g_ctx.socket_fd, SHUT_RDWR);
    }
    if (g_ctx.active) {
        threadWaitForExit(&g_ctx.thread);
        threadClose(&g_ctx.thread);
        g_ctx.active = false;
    }
}

bool mqtt_test_connection(AppState *state) {
    AppConfig config;
    mutexLock(&state->lock);
    config = state->config;
    mutexUnlock(&state->lock);

    char test_client_id[sizeof(config.mqtt_client_id)];
    snprintf(test_client_id, sizeof(test_client_id), "%.58s-test", mqtt_client_id_or_device(&config));
    strncpy(config.mqtt_client_id, test_client_id, sizeof(config.mqtt_client_id) - 1);
    config.mqtt_client_id[sizeof(config.mqtt_client_id) - 1] = '\0';

    char host[SHA_MAX_FIELD];
    int port = config.mqtt_port;
    mqtt_normalize_endpoint(&config, host, sizeof(host), &port);

    if (host[0] == '\0') {
        mqtt_set_status(state, false, false, false, "MQTT host missing");
        app_state_push_log(state, "MQTT test failed: host missing");
        return false;
    }
    if (port <= 0 || port > 65535) {
        mqtt_set_status(state, false, false, false, "Invalid MQTT port: %d", port);
        app_state_push_log(state, "MQTT test failed: invalid port");
        return false;
    }
    if (port == 8123) {
        mqtt_set_status(state, true, false, false, "Port 8123 is HA HTTP; MQTT is usually 1883");
        app_state_push_log(state, "MQTT test failed: port 8123");
        return false;
    }

    int fd = -1;
    app_state_push_log(state, "MQTT test %s:%d", host, port);
    if (!mqtt_socket_connect_to(state, host, port, &fd)) {
        mutexLock(&state->lock);
        char error[SHA_LOG_LINE];
        strncpy(error, state->mqtt_last_error, sizeof(error) - 1);
        error[sizeof(error) - 1] = '\0';
        mutexUnlock(&state->lock);
        app_state_push_log(state, "MQTT test failed: %s", error);
        return false;
    }

    bool ok = false;
    if (!mqtt_send_connect(&config, fd)) {
        mqtt_set_status(state, true, true, false, "MQTT CONNECT send failed errno=%d", errno);
        app_state_push_log(state, "MQTT test failed: CONNECT send");
    } else {
        unsigned char ack[4] = {0};
        if (mqtt_socket_recv_all(fd, ack, sizeof(ack)) && ack[0] == 0x20 && ack[1] == 0x02 && ack[3] == 0x00) {
            mqtt_set_status(state, true, true, true, "MQTT test OK");
            app_state_push_log(state, "MQTT test OK");
            ok = true;
        } else {
            int return_code = ack[3];
            mqtt_set_status(state, true, true, false, "MQTT rejected rc=%d %s", return_code, mqtt_connack_reason(return_code));
            app_state_push_log(state, "MQTT test rejected rc=%d %s", return_code, mqtt_connack_reason(return_code));
        }
    }

    close(fd);
    return ok;
}
