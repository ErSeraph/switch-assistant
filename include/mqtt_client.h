#pragma once

#include "app_state.h"

bool mqtt_service_start(AppState *state);
void mqtt_service_stop(AppState *state);
bool mqtt_test_connection(AppState *state);
