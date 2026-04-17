#pragma once

#include <switch.h>
#include <stdbool.h>

#define SHA_MAX_FIELD 256
#define SHA_MAX_TOPIC 128
#define SHA_MAX_LOG 12
#define SHA_LOG_LINE 96
#define SHA_APP_BUILD "1.0"

typedef enum {
    AppMode_ForegroundApp = 0,
    AppMode_Sysmodule = 1,
} AppMode;

typedef struct {
    char ha_url[SHA_MAX_FIELD];
    char ha_token[SHA_MAX_FIELD];
    char mqtt_host[SHA_MAX_FIELD];
    int mqtt_port;
    char mqtt_username[SHA_MAX_FIELD];
    char mqtt_password[SHA_MAX_FIELD];
    char mqtt_topic_prefix[SHA_MAX_TOPIC];
    char mqtt_client_id[64];
    char device_name[64];
} AppConfig;

typedef struct {
    AppConfig config;
    Mutex lock;
    bool exit_requested;
    bool mqtt_running;
    bool mqtt_dns_ok;
    bool mqtt_tcp_ok;
    bool mqtt_connected;
    bool ha_validated;
    bool config_dirty;
    bool restart_required;
    bool svc_hid_ready;
    bool svc_lbl_ready;
    bool svc_audctl_ready;
    bool svc_spsm_ready;
    u64 last_heartbeat_ms;
    char status_line[SHA_LOG_LINE];
    char config_status[SHA_LOG_LINE];
    char mqtt_last_error[SHA_LOG_LINE];
    AppMode app_mode;
    char log_lines[SHA_MAX_LOG][SHA_LOG_LINE];
    int log_count;
} AppState;

void app_state_init(AppState *state);
void app_state_set_status(AppState *state, const char *fmt, ...);
void app_state_push_log(AppState *state, const char *fmt, ...);
