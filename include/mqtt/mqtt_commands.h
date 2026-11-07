#pragma once

#include "app_state.h"

#define NOTIFY_TEXT_MAX 512

void mqtt_handle_publish(AppState *state, int fd, const AppConfig *config, const unsigned char *buffer, int size);
void mqtt_process_overlay_commands(AppState *state, int fd, const AppConfig *config);
