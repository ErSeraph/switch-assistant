#include "mqtt/mqtt_topics.h"

#include <stdio.h>
#include <string.h>

void mqtt_sanitize_id(const char *input, char *output, size_t output_size) {
    size_t j = 0;
    for (size_t i = 0; input[i] && j + 1 < output_size; ++i) {
        char ch = input[i];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
            output[j++] = ch;
        } else {
            output[j++] = '_';
        }
    }
    output[j] = '\0';
}

const char *mqtt_client_id_or_device(const AppConfig *config) {
    return config->mqtt_client_id[0] ? config->mqtt_client_id : config->device_name;
}

void mqtt_state_topic(const AppConfig *config, const char *sensor, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/%s", safe_id, sensor);
}

void mqtt_discovery_topic(const AppConfig *config, const char *component, const char *sensor, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "%s/%s/%s/%s/config", config->mqtt_topic_prefix, component, safe_id, sensor);
}

void mqtt_notification_topic(const AppConfig *config, const char *mode, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/notify/%s", safe_id, mode);
}

void mqtt_notification_status_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/notify/status", safe_id);
}

void mqtt_command_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/command", safe_id);
}

void mqtt_remote_send_command_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/remote/send_command", safe_id);
}

void mqtt_remote_set_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/remote/set", safe_id);
}

void mqtt_remote_state_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/remote/state", safe_id);
}

void mqtt_media_player_state_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/media_player/state", safe_id);
}

void mqtt_media_player_command_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/media_player/cmd", safe_id);
}

void mqtt_media_player_volume_cmd_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/media_player/volume/set", safe_id);
}

void mqtt_media_player_volume_state_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/media_player/volume/state", safe_id);
}

void mqtt_media_player_title_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/media_player/title", safe_id);
}

void mqtt_media_player_attr_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/media_player/attributes", safe_id);
}

void mqtt_remote_activity_cmd_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/remote/activity/set", safe_id);
}

void mqtt_remote_activity_state_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/remote/activity/state", safe_id);
}

void mqtt_media_player_source_cmd_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/media_player/source/set", safe_id);
}

void mqtt_media_player_source_state_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/media_player/source/state", safe_id);
}

void mqtt_select_game_cmd_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/select_game/set", safe_id);
}

void mqtt_select_game_state_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/select_game/state", safe_id);
}

void mqtt_select_game_2p_cmd_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/select_game_2p/set", safe_id);
}

void mqtt_select_game_2p_state_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/select_game_2p/state", safe_id);
}

void mqtt_select_game_4p_cmd_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/select_game_4p/set", safe_id);
}

void mqtt_select_game_4p_state_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/select_game_4p/state", safe_id);
}

void mqtt_select_game_8p_cmd_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/select_game_8p/set", safe_id);
}

void mqtt_select_game_8p_state_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/select_game_8p/state", safe_id);
}

void mqtt_sensor_attr_topic(const AppConfig *config, const char *sensor, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/sensor/%s/attributes", safe_id, sensor);
}

void mqtt_update_state_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/update/firmware/state", safe_id);
}

void mqtt_update_command_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/update/firmware/install", safe_id);
}

void mqtt_atmosphere_update_state_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/update/atmosphere/state", safe_id);
}

void mqtt_atmosphere_update_command_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/update/atmosphere/install", safe_id);
}

void mqtt_overlay_event_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/event/action", safe_id);
}

void mqtt_call_service_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/call_service", safe_id);
}

void mqtt_availability_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/availability", safe_id);
}

void mqtt_brightness_cmd_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/brightness/set", safe_id);
}

void mqtt_brightness_state_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/brightness/state", safe_id);
}

void mqtt_backlight_cmd_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/backlight/set", safe_id);
}

void mqtt_backlight_state_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/backlight/state", safe_id);
}

void mqtt_diagnostics_state_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/diagnostics", safe_id);
}

void mqtt_diagnostics_attr_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/diagnostics/attributes", safe_id);
}

void mqtt_logs_response_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/logs/response", safe_id);
}

void mqtt_logs_command_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/command/get_logs", safe_id);
}


