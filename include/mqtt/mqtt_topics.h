#pragma once

#include "app_state.h"

#include <stddef.h>

void mqtt_sanitize_id(const char *input, char *output, size_t output_size);
const char *mqtt_client_id_or_device(const AppConfig *config);

void mqtt_state_topic(const AppConfig *config, const char *sensor, char *topic, size_t topic_size);
void mqtt_discovery_topic(const AppConfig *config, const char *component, const char *sensor, char *topic, size_t topic_size);

void mqtt_notification_topic(const AppConfig *config, const char *mode, char *topic, size_t topic_size);
void mqtt_notification_status_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_command_topic(const AppConfig *config, char *topic, size_t topic_size);

void mqtt_remote_send_command_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_remote_set_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_remote_state_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_remote_activity_cmd_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_remote_activity_state_topic(const AppConfig *config, char *topic, size_t topic_size);

void mqtt_media_player_state_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_media_player_command_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_media_player_volume_cmd_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_media_player_volume_state_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_media_player_title_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_media_player_attr_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_media_player_source_cmd_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_media_player_source_state_topic(const AppConfig *config, char *topic, size_t topic_size);

void mqtt_select_game_cmd_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_select_game_state_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_select_game_2p_cmd_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_select_game_2p_state_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_select_game_4p_cmd_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_select_game_4p_state_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_select_game_8p_cmd_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_select_game_8p_state_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_sensor_attr_topic(const AppConfig *config, const char *sensor, char *topic, size_t topic_size);

void mqtt_update_state_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_update_command_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_atmosphere_update_state_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_atmosphere_update_command_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_overlay_event_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_call_service_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_availability_topic(const AppConfig *config, char *topic, size_t topic_size);

void mqtt_brightness_cmd_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_brightness_state_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_backlight_cmd_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_backlight_state_topic(const AppConfig *config, char *topic, size_t topic_size);

void mqtt_diagnostics_state_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_diagnostics_attr_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_logs_response_topic(const AppConfig *config, char *topic, size_t topic_size);
void mqtt_logs_command_topic(const AppConfig *config, char *topic, size_t topic_size);

