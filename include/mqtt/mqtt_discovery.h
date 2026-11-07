#pragma once

#include "app_state.h"

void mqtt_publish_sensor_discovery(AppState *state, int fd, const AppConfig *config);
void mqtt_clear_discovery_topic(int fd, const AppConfig *config, const char *component, const char *sensor);
void mqtt_clear_old_discovery_topics(AppState *state, int fd, const AppConfig *config);
