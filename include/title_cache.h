#pragma once

#include <switch.h>
#include <stdbool.h>
#include <stddef.h>

#define TITLE_CACHE_PATH "sdmc:/switch/switch-ha/titles.txt"

bool title_cache_lookup(u64 application_id, char *out, size_t out_size);
