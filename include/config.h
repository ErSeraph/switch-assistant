#pragma once

#include "app_state.h"

bool config_load(AppConfig *config);
bool config_save(const AppConfig *config);
void config_set_defaults(AppConfig *config);
const char *config_path(void);
const char *config_last_error(void);
