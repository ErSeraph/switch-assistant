#pragma once

#include "app_state.h"

#include <switch.h>
#include <stdbool.h>
#include <stddef.h>

#define TITLE_CACHE_PATH "sdmc:/switch/switch-ha/titles.txt"
#define TITLE_INDEX_PATH "sdmc:/switch/switch-ha/titles.idx"
#define MAX_INSTALLED_GAMES 256

typedef struct {
    u64 application_id;
    char name[128];
    int max_players;
} InstalledGame;

void title_cache_init(void);
bool title_cache_lookup(u64 application_id, char *out, size_t out_size);
size_t title_cache_get_installed_games(InstalledGame *out_games, size_t max_games);
size_t title_cache_refresh_installed_games(void);
bool title_cache_find_application_id_by_name(const char *name, u64 *out_application_id);
bool title_cache_launch_game(AppState *state, u64 application_id);
int title_cache_get_max_players(u64 application_id, const char *name);
void title_cache_build_json_options_array(char *out, size_t out_size);
void title_cache_build_json_options_array_for_players(char *out, size_t out_size, int min_players);
size_t title_cache_get_games_by_min_players(InstalledGame *out_games, size_t max_games, int min_players);
void title_cache_build_json_games_attr(char *out, size_t out_size, int min_players);
