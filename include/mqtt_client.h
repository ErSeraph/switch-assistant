#pragma once

#include "app_state.h"

bool mqtt_service_start(AppState *state);
void mqtt_service_stop(AppState *state);
bool mqtt_test_connection(AppState *state);
// True when the MQTT thread holds no socket (not connected and not connecting).
bool mqtt_service_socket_idle(void);
// Forces a blocked MQTT socket to fail so the MQTT thread closes it (sleep path).
void mqtt_service_abort_socket(void);
