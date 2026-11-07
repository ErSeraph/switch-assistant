#pragma once

#include "app_state.h"

#include <stdbool.h>
#include <stddef.h>

#define NOTIFY_TEXT_MAX 512

void mqtt_trim_text(char *value);
void mqtt_copy_text(char *out, size_t out_size, const char *text);
bool mqtt_json_extract_string(const char *json, const char *key, char *out, size_t out_size);
bool mqtt_json_extract_int(const char *json, const char *key, int *out);
bool mqtt_write_notification_files(const char *mode, const char *title, const char *message);

bool mqtt_prepare_firmware_directory(void);
