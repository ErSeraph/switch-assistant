#include "mqtt/mqtt_discovery.h"
#include "mqtt/mqtt_proto.h"
#include "mqtt/mqtt_topics.h"
#include "mqtt/mqtt_state.h"

#include <switch.h>
#include <stdio.h>
#include <string.h>

static void get_system_device_info(AppState *state, char *mac_str, size_t mac_size, char *fw_str, size_t fw_size) {
    if (mac_str && mac_size > 0) {
        mac_str[0] = '\0';
        if (state && state->cached_mac[0] != '\0') {
            snprintf(mac_str, mac_size, "%s", state->cached_mac);
        }
    }

    if (fw_str && fw_size > 0) {
        const char *fw_ver = (state && state->cached_firmware_ver[0] != '\0') ? state->cached_firmware_ver : "Unknown";
        const char *ams_ver = (state && state->cached_atmosphere_ver[0] != '\0') ? state->cached_atmosphere_ver : "Unknown";

        // If ams_ver accidentally equals fw_ver (e.g. 23.0.1 == 23.0.1), discard it and use fallback
        char safe_ams[64] = "";
        if (strcmp(ams_ver, "Unknown") != 0 && strcmp(ams_ver, fw_ver) != 0 && strncmp(ams_ver, "23.", 3) != 0) {
            snprintf(safe_ams, sizeof(safe_ams), "%s", ams_ver);
        } else if (strncmp(fw_ver, "23.", 3) == 0) {
            snprintf(safe_ams, sizeof(safe_ams), "1.12.0");
        }

        if (safe_ams[0] != '\0' && strcmp(fw_ver, "Unknown") != 0) {
            snprintf(fw_str, fw_size, "%.32s (AMS %.32s)", fw_ver, safe_ams);
        } else if (strcmp(fw_ver, "Unknown") != 0) {
            snprintf(fw_str, fw_size, "%.64s", fw_ver);
        } else if (safe_ams[0] != '\0') {
            snprintf(fw_str, fw_size, "AMS %.64s", safe_ams);
        } else {
            snprintf(fw_str, fw_size, "Nintendo Switch");
        }
    }
}

static void build_device_json_snippet(AppState *state, const AppConfig *config, const char *safe_id, char *out_dev, size_t out_size) {
    char mac[32] = "";
    char sw_version[160] = "";
    char avty_top[192] = "";
    get_system_device_info(state, mac, sizeof(mac), sw_version, sizeof(sw_version));
    mqtt_availability_topic(config, avty_top, sizeof(avty_top));

    if (mac[0] != '\0') {
        snprintf(out_dev, out_size,
                 "\"avty_t\":\"%s\",\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"Nintendo\",\"mdl\":\"Switch\",\"sw_version\":\"%s\",\"connections\":[[\"mac\",\"%s\"]]}",
                 avty_top, safe_id, config->device_name, sw_version, mac);
    } else {
        snprintf(out_dev, out_size,
                 "\"avty_t\":\"%s\",\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"Nintendo\",\"mdl\":\"Switch\",\"sw_version\":\"%s\"}",
                 avty_top, safe_id, config->device_name, sw_version);
    }
}

static void publish_discovery_sensor_ext(AppState *state, int fd, const AppConfig *config, const char *component, const char *sensor,
                                         const char *name, const char *device_class, const char *unit, const char *state_class,
                                         const char *entity_category, const char *icon, bool enabled_by_default) {
    char topic[256];
    char state_topic_buf[192];
    char payload[MQTT_BUFFER];
    char safe_id[80];
    char dev_json[512];

    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    build_device_json_snippet(state, config, safe_id, dev_json, sizeof(dev_json));
    mqtt_discovery_topic(config, component, sensor, topic, sizeof(topic));
    mqtt_state_topic(config, sensor, state_topic_buf, sizeof(state_topic_buf));

    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"stat_t\":\"%s\",%s%s%s%s%s}",
             name,
             safe_id,
             sensor,
             state_topic_buf,
             dev_json,
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
    if (device_class && strcmp(device_class, "data_size") == 0) {
        // Sizes are whole MB; without this Home Assistant shows them with
        // the data_size default of two decimals.
        size_t len = strlen(payload);
        if (len > 0 && payload[len - 1] == '}') {
            payload[len - 1] = '\0';
            snprintf(payload + strlen(payload), sizeof(payload) - strlen(payload), ",\"suggested_display_precision\":0}");
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

static void publish_discovery_sensor(AppState *state, int fd, const AppConfig *config, const char *component, const char *sensor,
                                     const char *name, const char *device_class, const char *unit, const char *state_class) {
    publish_discovery_sensor_ext(state, fd, config, component, sensor, name, device_class, unit, state_class, NULL, NULL, true);
}

static void publish_discovery_sensor_disabled(AppState *state, int fd, const AppConfig *config, const char *component, const char *sensor,
                                              const char *name, const char *device_class, const char *unit, const char *state_class,
                                              const char *entity_category, const char *icon) {
    publish_discovery_sensor_ext(state, fd, config, component, sensor, name, device_class, unit, state_class, entity_category, icon, false);
}

static void publish_discovery_sensor_with_attr(AppState *state, int fd, const AppConfig *config, const char *sensor,
                                               const char *name, const char *icon, const char *entity_category) {
    char topic[256];
    char state_topic_buf[192];
    char attr_topic_buf[192];
    char payload[MQTT_BUFFER];
    char safe_id[80];
    char dev_json[512];

    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    build_device_json_snippet(state, config, safe_id, dev_json, sizeof(dev_json));
    mqtt_discovery_topic(config, "sensor", sensor, topic, sizeof(topic));
    mqtt_diagnostics_state_topic(config, state_topic_buf, sizeof(state_topic_buf));
    mqtt_diagnostics_attr_topic(config, attr_topic_buf, sizeof(attr_topic_buf));

    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"stat_t\":\"%s\",\"json_attr_t\":\"%s\","
             "\"ic\":\"%s\",\"ent_cat\":\"%s\",%s}",
             name,
             safe_id,
             sensor,
             state_topic_buf,
             attr_topic_buf,
             icon ? icon : "mdi:information",
             entity_category ? entity_category : "diagnostic",
             dev_json);

    mqtt_send_publish(fd, topic, payload, true);
}

static void publish_discovery_button(AppState *state, int fd, const AppConfig *config, const char *button, const char *name, const char *payload_press) {
    char topic[256];
    char command_topic[SHA_MAX_TOPIC + 32];
    char payload[MQTT_BUFFER];
    char safe_id[80];
    char dev_json[512];

    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    build_device_json_snippet(state, config, safe_id, dev_json, sizeof(dev_json));
    mqtt_discovery_topic(config, "button", button, topic, sizeof(topic));
    mqtt_command_topic(config, command_topic, sizeof(command_topic));

    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"cmd_t\":\"%s\",\"payload_press\":\"%s\",%s}",
             name,
             safe_id,
             button,
             command_topic,
             payload_press,
             dev_json);

    mqtt_send_publish(fd, topic, payload, true);
}

static void publish_discovery_notify(AppState *state, int fd, const AppConfig *config, const char *mode, const char *name) {
    char topic[256];
    char command_topic[192];
    char payload[MQTT_BUFFER];
    char safe_id[80];
    char dev_json[512];

    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    build_device_json_snippet(state, config, safe_id, dev_json, sizeof(dev_json));
    mqtt_discovery_topic(config, "notify", mode, topic, sizeof(topic));
    mqtt_notification_topic(config, mode, command_topic, sizeof(command_topic));

    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"uniq_id\":\"%s_notify_%s\",\"cmd_t\":\"%s\","
             "\"cmd_tpl\":\"{\\\"message\\\":\\\"{{ message }}\\\",\\\"title\\\":\\\"{{ title | default('') }}\\\"}\","
             "\"schema\":\"json\","
             "\"ic\":\"mdi:bell-ring\",%s}",
             name,
             safe_id,
             mode,
             command_topic,
             dev_json);

    mqtt_send_publish(fd, topic, payload, true);
}

static void publish_discovery_number(AppState *state, int fd, const AppConfig *config, const char *number_id,
                                     const char *name, const char *cmd_topic, const char *stat_topic,
                                     float min, float max, float step, const char *unit, const char *icon) {
    char topic[256];
    char payload[MQTT_BUFFER];
    char safe_id[80];
    char dev_json[512];

    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    build_device_json_snippet(state, config, safe_id, dev_json, sizeof(dev_json));
    mqtt_discovery_topic(config, "number", number_id, topic, sizeof(topic));

    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"uniq_id\":\"%s_num_%s\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
             "\"min\":%.1f,\"max\":%.1f,\"step\":%.1f%s%s%s%s%s%s,%s}",
             name,
             safe_id,
             number_id,
             cmd_topic,
             stat_topic,
             min, max, step,
             unit ? ",\"unit_of_meas\":\"" : "",
             unit ? unit : "",
             unit ? "\"" : "",
             icon ? ",\"ic\":\"" : "",
             icon ? icon : "",
             icon ? "\"" : "",
             dev_json);

    mqtt_send_publish(fd, topic, payload, true);
}

static void publish_discovery_switch(AppState *state, int fd, const AppConfig *config, const char *switch_id,
                                     const char *name, const char *cmd_topic, const char *stat_topic,
                                     const char *icon) {
    char topic[256];
    char payload[MQTT_BUFFER];
    char safe_id[80];
    char dev_json[512];

    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    build_device_json_snippet(state, config, safe_id, dev_json, sizeof(dev_json));
    mqtt_discovery_topic(config, "switch", switch_id, topic, sizeof(topic));

    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"uniq_id\":\"%s_sw_%s\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
             "\"pl_on\":\"ON\",\"pl_off\":\"OFF\"%s%s%s,%s}",
             name,
             safe_id,
             switch_id,
             cmd_topic,
             stat_topic,
             icon ? ",\"ic\":\"" : "",
             icon ? icon : "",
             icon ? "\"" : "",
             dev_json);

    mqtt_send_publish(fd, topic, payload, true);
}

#include "title_cache.h"
#include <stdlib.h>

static void publish_discovery_remote(AppState *state, int fd, const AppConfig *config) {
    char topic[256];
    char send_cmd_topic[192];
    char cmd_set_topic[192];
    char state_top[192];
    char act_cmd_top[192];
    char act_stat_top[192];
    char safe_id[80];
    char dev_json[512];

    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    build_device_json_snippet(state, config, safe_id, dev_json, sizeof(dev_json));
    mqtt_discovery_topic(config, "remote", "remote", topic, sizeof(topic));
    mqtt_remote_send_command_topic(config, send_cmd_topic, sizeof(send_cmd_topic));
    mqtt_remote_set_topic(config, cmd_set_topic, sizeof(cmd_set_topic));
    mqtt_remote_state_topic(config, state_top, sizeof(state_top));
    mqtt_remote_activity_cmd_topic(config, act_cmd_top, sizeof(act_cmd_top));
    mqtt_remote_activity_state_topic(config, act_stat_top, sizeof(act_stat_top));

    char *options_json = malloc(16384);
    if (!options_json) {
        return;
    }
    title_cache_build_json_options_array(options_json, 16384);

    size_t payload_size = 16384 + 2048;
    char *payload = malloc(payload_size);
    if (!payload) {
        free(options_json);
        return;
    }

    snprintf(payload, payload_size,
             "{\"name\":\"Remote\",\"uniq_id\":\"%s_remote\","
             "\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
             "\"send_cmd_t\":\"%s\","
             "\"act_cmd_t\":\"%s\",\"act_stat_t\":\"%s\","
             "\"act_list\":%s,%s}",
             safe_id,
             cmd_set_topic,
             state_top,
             send_cmd_topic,
             act_cmd_top,
             act_stat_top,
             options_json,
             dev_json);

    mqtt_send_publish(fd, topic, payload, true);
    free(payload);
    free(options_json);
}

static void publish_discovery_media_player(AppState *state, int fd, const AppConfig *config) {
    char topic[256];
    char cmd_topic[192];
    char stat_topic[192];
    char vol_cmd_top[192];
    char vol_stat_top[192];
    char src_cmd_top[192];
    char src_stat_top[192];
    char title_stat_top[192];
    char attr_stat_top[192];
    char safe_id[80];
    char dev_json[512];

    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    build_device_json_snippet(state, config, safe_id, dev_json, sizeof(dev_json));
    mqtt_discovery_topic(config, "media_player", "media_player", topic, sizeof(topic));
    mqtt_media_player_command_topic(config, cmd_topic, sizeof(cmd_topic));
    mqtt_media_player_state_topic(config, stat_topic, sizeof(stat_topic));
    mqtt_media_player_volume_cmd_topic(config, vol_cmd_top, sizeof(vol_cmd_top));
    mqtt_media_player_volume_state_topic(config, vol_stat_top, sizeof(vol_stat_top));
    mqtt_media_player_source_cmd_topic(config, src_cmd_top, sizeof(src_cmd_top));
    mqtt_media_player_source_state_topic(config, src_stat_top, sizeof(src_stat_top));
    mqtt_media_player_title_topic(config, title_stat_top, sizeof(title_stat_top));
    mqtt_media_player_attr_topic(config, attr_stat_top, sizeof(attr_stat_top));

    char *options_json = malloc(16384);
    if (!options_json) {
        return;
    }
    title_cache_build_json_options_array(options_json, 16384);

    size_t payload_size = 16384 + 4096;
    char *payload = malloc(payload_size);
    if (!payload) {
        free(options_json);
        return;
    }

    snprintf(payload, payload_size,
             "{\"name\":\"Media Player\",\"uniq_id\":\"%s_media_player\","
             "\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
             "\"vol_cmd_t\":\"%s\",\"vol_stat_t\":\"%s\","
             "\"src_cmd_t\":\"%s\",\"src_stat_t\":\"%s\","
             "\"title_stat_t\":\"%s\","
             "\"json_attr_t\":\"%s\","
             "\"source_list\":%s,"
             "\"dev_cla\":\"tv\","
             "\"sup_feat\":[\"turn_on\",\"turn_off\",\"volume_set\",\"volume_step\",\"volume_mute\",\"select_source\"],%s}",
             safe_id,
             cmd_topic,
             stat_topic,
             vol_cmd_top,
             vol_stat_top,
             src_cmd_top,
             src_stat_top,
             title_stat_top,
             attr_stat_top,
             options_json,
             dev_json);

    mqtt_send_publish(fd, topic, payload, true);
    free(payload);
    free(options_json);
}

static void publish_discovery_select_game(AppState *state, int fd, const AppConfig *config) {
    char topic[256];
    char cmd_topic[192];
    char stat_topic[192];
    char safe_id[80];
    char dev_json[512];

    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    build_device_json_snippet(state, config, safe_id, dev_json, sizeof(dev_json));
    mqtt_discovery_topic(config, "select", "game", topic, sizeof(topic));
    mqtt_select_game_cmd_topic(config, cmd_topic, sizeof(cmd_topic));
    mqtt_select_game_state_topic(config, stat_topic, sizeof(stat_topic));

    char *options_json = malloc(16384);
    if (!options_json) {
        return;
    }
    title_cache_build_json_options_array(options_json, 16384);

    size_t payload_size = 16384 + 2048;
    char *payload = malloc(payload_size);
    if (!payload) {
        free(options_json);
        return;
    }

    snprintf(payload, payload_size,
             "{\"name\":\"Channel\",\"uniq_id\":\"%s_select_game\","
             "\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
             "\"options\":%s,"
             "\"ic\":\"mdi:television-box\",%s}",
             safe_id,
             cmd_topic,
             stat_topic,
             options_json,
             dev_json);

    mqtt_send_publish(fd, topic, payload, true);
    free(payload);
    free(options_json);
}

static void publish_discovery_select_game_players(AppState *state, int fd, const AppConfig *config, const char *sensor, const char *name, int min_players, const char *icon) {
    char topic[256];
    char cmd_topic[192];
    char stat_topic[192];
    char safe_id[80];
    char dev_json[512];

    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    build_device_json_snippet(state, config, safe_id, dev_json, sizeof(dev_json));
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

    char *options_json = malloc(16384);
    if (!options_json) {
        return;
    }
    title_cache_build_json_options_array_for_players(options_json, 16384, min_players);

    size_t payload_size = 16384 + 2048;
    char *payload = malloc(payload_size);
    if (!payload) {
        free(options_json);
        return;
    }

    snprintf(payload, payload_size,
             "{\"name\":\"%s\",\"uniq_id\":\"%s_select_%s\","
             "\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
             "\"options\":%s,"
             "\"ic\":\"%s\",%s}",
             name,
             safe_id,
             sensor,
             cmd_topic,
             stat_topic,
             options_json,
             icon,
             dev_json);

    mqtt_send_publish(fd, topic, payload, true);
    free(payload);
    free(options_json);
}

static void publish_discovery_sensor_games_count(AppState *state, int fd, const AppConfig *config, const char *sensor, const char *name, const char *icon) {
    char topic[256];
    char state_topic_buf[192];
    char attr_topic[192];
    char payload[MQTT_BUFFER];
    char safe_id[80];
    char dev_json[512];

    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    build_device_json_snippet(state, config, safe_id, dev_json, sizeof(dev_json));
    mqtt_discovery_topic(config, "sensor", sensor, topic, sizeof(topic));
    mqtt_state_topic(config, sensor, state_topic_buf, sizeof(state_topic_buf));
    mqtt_sensor_attr_topic(config, sensor, attr_topic, sizeof(attr_topic));

    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"stat_t\":\"%s\",\"json_attr_t\":\"%s\",\"ic\":\"%s\",%s}",
             name,
             safe_id,
             sensor,
             state_topic_buf,
             attr_topic,
             icon,
             dev_json);

    mqtt_send_publish(fd, topic, payload, true);
}

static void publish_discovery_update(AppState *state, int fd, const AppConfig *config) {
    char topic[256];
    char state_top[192];
    char cmd_top[192];
    char payload[MQTT_BUFFER];
    char safe_id[80];
    char dev_json[512];

    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    build_device_json_snippet(state, config, safe_id, dev_json, sizeof(dev_json));
    mqtt_discovery_topic(config, "update", "firmware", topic, sizeof(topic));
    mqtt_update_state_topic(config, state_top, sizeof(state_top));
    mqtt_update_command_topic(config, cmd_top, sizeof(cmd_top));

    snprintf(payload, sizeof(payload),
             "{\"name\":\"Firmware\",\"uniq_id\":\"%s_firmware\",\"stat_t\":\"%s\",\"cmd_t\":\"%s\","
             "\"title\":\"Nintendo Switch Firmware\",\"dev_cla\":\"firmware\","
             "\"release_url\":\"https://github.com/THZoria/NX_Firmware/releases\","
             "\"entity_picture\":\"https://raw.githubusercontent.com/FaserF/switch-assistant/main/assets/icon.jpg\",%s}",
             safe_id,
             state_top,
             cmd_top,
             dev_json);

    mqtt_send_publish(fd, topic, payload, true);
}

static void publish_discovery_atmosphere_update(AppState *state, int fd, const AppConfig *config) {
    char topic[256];
    char state_top[192];
    char cmd_top[192];
    char payload[MQTT_BUFFER];
    char safe_id[80];
    char dev_json[512];

    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    build_device_json_snippet(state, config, safe_id, dev_json, sizeof(dev_json));
    mqtt_discovery_topic(config, "update", "atmosphere", topic, sizeof(topic));
    mqtt_atmosphere_update_state_topic(config, state_top, sizeof(state_top));
    mqtt_atmosphere_update_command_topic(config, cmd_top, sizeof(cmd_top));

    snprintf(payload, sizeof(payload),
             "{\"name\":\"Atmosphère\",\"uniq_id\":\"%s_atmosphere\","
             "\"stat_t\":\"%s\",\"cmd_t\":\"%s\","
             "\"title\":\"Atmosphère Custom Firmware\",\"dev_cla\":\"firmware\","
             "\"release_url\":\"https://github.com/Atmosphere-NX/Atmosphere/releases/latest\","
             "\"entity_picture\":\"https://avatars.githubusercontent.com/u/39328464\",%s}",
             safe_id,
             state_top,
             cmd_top,
             dev_json);

    mqtt_send_publish(fd, topic, payload, true);
}

static void publish_discovery_event(AppState *state, int fd, const AppConfig *config) {
    char topic[256];
    char state_top[192];
    char payload[MQTT_BUFFER];
    char safe_id[80];
    char dev_json[512];

    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    build_device_json_snippet(state, config, safe_id, dev_json, sizeof(dev_json));
    mqtt_discovery_topic(config, "event", "overlay_action", topic, sizeof(topic));
    mqtt_overlay_event_topic(config, state_top, sizeof(state_top));

    snprintf(payload, sizeof(payload),
             "{\"name\":\"Overlay Action\",\"uniq_id\":\"%s_overlay_action\","
             "\"stat_t\":\"%s\",\"event_types\":[\"toggle\",\"turn_on\",\"turn_off\",\"press\",\"call_service\",\"quick_action_1\",\"quick_action_2\",\"quick_action_3\",\"quick_action_4\"],%s}",
             safe_id,
             state_top,
             dev_json);

    mqtt_send_publish(fd, topic, payload, true);
}

void mqtt_clear_discovery_topic(int fd, const AppConfig *config, const char *component, const char *sensor) {
    char topic[256];
    mqtt_discovery_topic(config, component, sensor, topic, sizeof(topic));
    mqtt_send_publish(fd, topic, "", true);
}

void mqtt_clear_old_discovery_topics(AppState *state, int fd, const AppConfig *config) {
    // Clear retained discovery topics left over from old firmware versions
    // that used a different device/client ID (e.g. "wohnzimmer_nintendo_switch").
    // Publishing an empty retained payload removes the config from HA.
    (void)state;

    static const char *old_ids[] = {
        "wohnzimmer_nintendo_switch",
        NULL
    };

    struct { const char *comp; const char *sensor; } old_entities[] = {
        {"sensor",        "diagnostics_logs"},
        {"sensor",        "diagnostics"},
        {"sensor",        "battery_temperature"},
        {"button",        "close_game"},
        {"button",        "fetch_diagnostics_logs"},
        {"button",        "get_logs"},
        {"button",        "sleep"},
        {"button",        "reboot"},
        {"button",        "shutdown"},
        {"switch",        "screen"},
        {"number",        "screen_brightness"},
        {"binary_sensor", "player_1_wired"},
        {"binary_sensor", "player_2_wired"},
        {"binary_sensor", "player_3_wired"},
        {"binary_sensor", "player_4_wired"},
        {"binary_sensor", "player_5_wired"},
        {"binary_sensor", "player_6_wired"},
        {"binary_sensor", "player_7_wired"},
        {"binary_sensor", "player_8_wired"},
        {NULL, NULL}
    };

    const char *prefixes[] = {
        "homeassistant",
        config->mqtt_topic_prefix,
        NULL
    };

    static bool s_already_cleared = false;
    if (s_already_cleared) {
        return;
    }
    s_already_cleared = true;

    for (int p_idx = 0; prefixes[p_idx] != NULL; p_idx++) {
        const char *disc_prefix = prefixes[p_idx];
        if (!disc_prefix || !disc_prefix[0]) continue;
        // Avoid duplicate clearance if config->mqtt_topic_prefix is already homeassistant
        if (p_idx > 0 && strcmp(prefixes[0], disc_prefix) == 0) continue;

        for (int p = 0; old_ids[p] != NULL; p++) {
            for (int e = 0; old_entities[e].comp != NULL; e++) {
                char old_topic[256];
                // Format 1: homeassistant/<component>/<device_id>/<sensor>/config
                snprintf(old_topic, sizeof(old_topic),
                         "%s/%s/%s/%s/config",
                         disc_prefix,
                         old_entities[e].comp,
                         old_ids[p],
                         old_entities[e].sensor);
                mqtt_send_publish(fd, old_topic, "", true);

                // Format 2: homeassistant/<component>/<device_id>_<sensor>/config
                snprintf(old_topic, sizeof(old_topic),
                         "%s/%s/%s_%s/config",
                         disc_prefix,
                         old_entities[e].comp,
                         old_ids[p],
                         old_entities[e].sensor);
                mqtt_send_publish(fd, old_topic, "", true);

                svcSleepThread(5ULL * 1000ULL * 1000ULL); // 5ms pacing to prevent socket buffer exhaustion
            }
        }
    }
}

void mqtt_publish_sensor_discovery(AppState *state, int fd, const AppConfig *config) {
    // Home Assistant Battery Integration (Core battery % and charging stay enabled)
    publish_discovery_sensor_ext(state, fd, config, "sensor", "battery", "Battery", "battery", "%", "measurement", NULL, NULL, true);
    publish_discovery_sensor_ext(state, fd, config, "binary_sensor", "charging", "Battery Charging", "battery_charging", NULL, NULL, NULL, NULL, true);

    // Advanced battery telemetry -> disabled by default
    publish_discovery_sensor_disabled(state, fd, config, "sensor", "charger_type", "Charger Type", NULL, NULL, NULL, "diagnostic", "mdi:power-plug");
    publish_discovery_sensor_disabled(state, fd, config, "sensor", "battery_voltage", "Battery Voltage", "voltage", "mV", "measurement", "diagnostic", "mdi:sine-wave");
    publish_discovery_sensor_disabled(state, fd, config, "sensor", "battery_temperature", "Battery Temperature", "temperature", "\xc2\xb0" "C", "measurement", "diagnostic", "mdi:thermometer");
    publish_discovery_sensor_disabled(state, fd, config, "sensor", "battery_health", "Battery Health", NULL, "%", "measurement", "diagnostic", "mdi:battery-heart-variant");

    if (state->svc_lbl_ready) {
        char br_cmd_topic[192];
        char br_stat_topic[192];
        char bl_cmd_topic[192];
        char bl_stat_topic[192];
        mqtt_brightness_cmd_topic(config, br_cmd_topic, sizeof(br_cmd_topic));
        mqtt_brightness_state_topic(config, br_stat_topic, sizeof(br_stat_topic));
        mqtt_backlight_cmd_topic(config, bl_cmd_topic, sizeof(bl_cmd_topic));
        mqtt_backlight_state_topic(config, bl_stat_topic, sizeof(bl_stat_topic));

        publish_discovery_number(state, fd, config, "brightness", "Screen Brightness", br_cmd_topic, br_stat_topic, 0.0f, 100.0f, 5.0f, "%", "mdi:brightness-6");
        publish_discovery_switch(state, fd, config, "backlight", "Screen", bl_cmd_topic, bl_stat_topic, "mdi:television-ambient-light");

        publish_discovery_sensor_disabled(state, fd, config, "sensor", "brightness", "Screen Brightness Sensor", NULL, "%", "measurement", "diagnostic", "mdi:brightness-6");
        publish_discovery_sensor_disabled(state, fd, config, "binary_sensor", "backlight", "Screen Backlight Sensor", NULL, NULL, NULL, "diagnostic", "mdi:television-ambient-light");
    }
    if (state->svc_audctl_ready) {
        publish_discovery_sensor_disabled(state, fd, config, "sensor", "volume", "Volume", NULL, "%", "measurement", "diagnostic", "mdi:volume-high");
        publish_discovery_sensor_disabled(state, fd, config, "sensor", "audio_target", "Audio Output Target", NULL, NULL, NULL, "diagnostic", "mdi:speaker");
    }
    publish_discovery_sensor(state, fd, config, "binary_sensor", "game_running", "Game Running", "running", NULL, NULL);
    publish_discovery_sensor(state, fd, config, "sensor", "current_game", "Current Game", NULL, NULL, NULL);
    publish_discovery_sensor_disabled(state, fd, config, "sensor", "current_game_id", "Current Game Title ID", NULL, NULL, NULL, "diagnostic", "mdi:identifier");
    mqtt_clear_discovery_topic(fd, config, "sensor", "current_game_name");
    mqtt_clear_discovery_topic(fd, config, "sensor", "game_pid");
    if (state->svc_hid_ready) {
        publish_discovery_sensor(state, fd, config, "sensor", "controller_count", "Player Count", NULL, NULL, "measurement");
        for (int i = 0; i < MAX_PLAYERS; ++i) {
            char sensor[32];
            char name[32];
            char wired_sensor[32];
            char wired_name[32];
            snprintf(sensor, sizeof(sensor), "player_%d_controller", i + 1);
            snprintf(name, sizeof(name), "Player %d Controller", i + 1);
            publish_discovery_sensor_disabled(state, fd, config, "sensor", sensor, name, NULL, NULL, NULL, "diagnostic", "mdi:controller");
            snprintf(wired_sensor, sizeof(wired_sensor), "player_%d_wired", i + 1);
            snprintf(wired_name, sizeof(wired_name), "Player %d Controller Wired", i + 1);
            publish_discovery_sensor_disabled(state, fd, config, "binary_sensor", wired_sensor, wired_name, "plug", NULL, NULL, "diagnostic", "mdi:controller-classic");
        }
    }
    if (state->svc_spsm_ready) {
        publish_discovery_button(state, fd, config, "reboot", "Reboot", "reboot");
        publish_discovery_button(state, fd, config, "shutdown", "Shutdown", "shutdown");
        publish_discovery_button(state, fd, config, "sleep", "Sleep", "sleep");
    } else {
        // Fallback: sysmodule can also use svcSleepSystem directly
        publish_discovery_button(state, fd, config, "sleep", "Sleep", "sleep");
    }
    publish_discovery_button(state, fd, config, "close_game", "Close Game", "close_game");
    publish_discovery_button(state, fd, config, "launch_daybreak", "Launch Daybreak", "launch_daybreak");
    publish_discovery_button(state, fd, config, "get_logs", "Fetch Diagnostics & Logs", "get_logs");
    publish_discovery_sensor_with_attr(state, fd, config, "diagnostics", "Diagnostics & Logs", "mdi:clipboard-pulse-outline", "diagnostic");

    // Publish critical system update entities before heavy installed games scan
    publish_discovery_update(state, fd, config);
    publish_discovery_atmosphere_update(state, fd, config);

    title_cache_refresh_installed_games();
    publish_discovery_remote(state, fd, config);
    publish_discovery_media_player(state, fd, config);
    publish_discovery_select_game(state, fd, config);
    publish_discovery_select_game_players(state, fd, config, "game_2p", "2-Player Games", 2, "mdi:account-multiple");
    publish_discovery_select_game_players(state, fd, config, "game_4p", "4-Player Games", 4, "mdi:account-group");
    publish_discovery_select_game_players(state, fd, config, "game_8p", "8+ Player Games", 8, "mdi:account-group-outline");
    publish_discovery_sensor_games_count(state, fd, config, "games_2p", "2-Player Games", "mdi:account-multiple");
    publish_discovery_sensor_games_count(state, fd, config, "games_4p", "4-Player Games", "mdi:account-group");
    publish_discovery_sensor_games_count(state, fd, config, "games_8p", "8+ Player Games", "mdi:account-group-outline");
    publish_discovery_event(state, fd, config);
    publish_discovery_sensor(state, fd, config, "binary_sensor", "docked", "Docked", "plug", NULL, NULL);
    if (state->svc_ts_ready) {
        publish_discovery_sensor_disabled(state, fd, config, "sensor", "soc_temperature", "SoC Temperature", "temperature", "\xc2\xb0" "C", "measurement", "diagnostic", "mdi:thermometer-lines");
    }
    publish_discovery_sensor_disabled(state, fd, config, "sensor", "uptime", "Uptime", "duration", "s", "total_increasing", "diagnostic", "mdi:clock-outline");
    if (state->svc_nifm_ready) {
        publish_discovery_sensor_disabled(state, fd, config, "sensor", "wifi_rssi", "WiFi Signal Strength", "signal_strength", "dBm", "measurement", "diagnostic", "mdi:wifi");
    }
    publish_discovery_sensor_disabled(state, fd, config, "sensor", "sd_free", "SD Card Free Space", "data_size", "MB", "measurement", "diagnostic", "mdi:micro-sd");
    publish_discovery_sensor_disabled(state, fd, config, "sensor", "sd_total", "SD Card Total Space", "data_size", "MB", "measurement", "diagnostic", "mdi:micro-sd");
    publish_discovery_sensor_disabled(state, fd, config, "sensor", "internal_free", "Internal Storage Free Space", "data_size", "MB", "measurement", "diagnostic", "mdi:harddisk");
    publish_discovery_sensor_disabled(state, fd, config, "sensor", "internal_total", "Internal Storage Total Space", "data_size", "MB", "measurement", "diagnostic", "mdi:harddisk");
    publish_discovery_sensor_disabled(state, fd, config, "sensor", "memory_used", "RAM Used", "data_size", "MB", "measurement", "diagnostic", "mdi:memory");
    if (state->app_mode == AppMode_Sysmodule) {
        publish_discovery_sensor_disabled(state, fd, config, "sensor", SCREEN_RTSP_URL_SENSOR, "Screen Stream URL", NULL, NULL, NULL, "diagnostic", "mdi:video");
        mqtt_clear_discovery_topic(fd, config, "sensor", "screen_stream_status");
        mqtt_clear_discovery_topic(fd, config, "sensor", "screen_rtsp_port");
        mqtt_clear_discovery_topic(fd, config, "camera", "screen_camera");
    }
    if (config->notification_overlay_enabled) {
        publish_discovery_notify(state, fd, config, "popup", "Popup Notification");
    } else {
        mqtt_clear_discovery_topic(fd, config, "notify", "popup");
    }
    mqtt_clear_discovery_topic(fd, config, "notify", "modal");
}
