#pragma once

#include "app_state.h"

#include <stdbool.h>
#include <stddef.h>

#define MAX_PLAYERS 8
#define SCREEN_RTSP_URL_SENSOR "screen_rtsp_url"

#define SENSOR_TOPIC_CACHE_SIZE 48

typedef struct {
    bool initialized;
    char battery[16];
    char charging[8];
    char charger_type[32];
    char battery_voltage[16];
    char battery_temperature[16];
    char battery_health[16];
    char brightness[16];
    char backlight[16];
    char volume[16];
    char audio_target[24];
    char game_running[8];
    char current_game_id[24];
    char current_game_name[0x200];
    char screen_rtsp_url[128];
    char player_controller_count[16];
    char player_controller[MAX_PLAYERS][32];
    char player_wired[MAX_PLAYERS][8];
    char docked[8];
    char soc_temperature[16];
    char uptime[24];
    char wifi_rssi[16];
    char sd_free[16];
    char sd_total[16];
    char internal_free[16];
    char internal_total[16];
    char memory_used[16];
    char mp_state[16];
    char mp_volume[16];
    char mp_title[0x200];
    char mp_source[0x200];
    int games_count_2p;
    int games_count_4p;
    int games_count_8p;
    // Last payload hash per state topic, so retained topics are only
    // re-published when their value changes (reset on every connection).
    struct {
        u32 topic_hash;
        u32 payload_hash;
    } topic_cache[SENSOR_TOPIC_CACHE_SIZE];
    int topic_cache_count;
} SensorSnapshot;

void mqtt_publish_switch_sensors(AppState *state, int fd, const AppConfig *config, SensorSnapshot *snapshot, bool force);
// Minimal "console is off" states published right before disconnecting for
// sleep/shutdown: no service calls, only a few small publishes.
void mqtt_publish_sleep_states(int fd, const AppConfig *config);
void mqtt_publish_state(AppState *state, int fd, const AppConfig *config, const char *status);
void mqtt_get_system_versions(AppState *state, char *fw_ver, size_t fw_size, char *ams_ver, size_t ams_size);
