#pragma once

#include "app_state.h"

#include <switch.h>
#include <stdbool.h>
#include <stddef.h>

bool screen_stream_start(AppState *state);
void screen_stream_stop(void);
void screen_stream_set_paused(bool paused);

u16 screen_stream_port(void);
void screen_stream_get_status(char *out, size_t out_size);
Result screen_stream_get_grcd_open_result(void);
Result screen_stream_get_grcd_begin_result(void);
