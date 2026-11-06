#include "logger.h"
#include "mqtt/mqtt_state.h"
#include "mqtt/mqtt_topics.h"
#include "title_cache.h"
#include "svc_session.h"

#include <switch.h>
#include <switch/services/applet.h>
#include <switch/services/ts.h>
#include <switch/services/nifm.h>
#include <switch/services/fs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static LogEntry g_log_ring[LOG_RING_CAPACITY];
static size_t g_log_count = 0;
static size_t g_log_head = 0;
static Mutex g_logger_mutex;
static bool g_logger_initialized = false;
static char g_log_file_path[128] = "sdmc:/switch/switch-ha/sysmodule.log";
static AppState *g_app_state = NULL;
static u32 g_log_write_counter = 0;

static const char *LEVEL_NAMES[] = {
    "DEBUG",
    "INFO",
    "WARN",
    "ERROR"
};

const char *logger_level_name(LogLevel level) {
    if (level >= LOG_LEVEL_DEBUG && level <= LOG_LEVEL_ERROR) {
        return LEVEL_NAMES[level];
    }
    return "UNKNOWN";
}

LogLevel logger_parse_level(const char *str) {
    if (!str || str[0] == '\0') return LOG_LEVEL_INFO;
    if (strcasecmp(str, "debug") == 0) return LOG_LEVEL_DEBUG;
    if (strcasecmp(str, "info") == 0)  return LOG_LEVEL_INFO;
    if (strcasecmp(str, "warn") == 0 || strcasecmp(str, "warning") == 0) return LOG_LEVEL_WARN;
    if (strcasecmp(str, "error") == 0 || strcasecmp(str, "err") == 0) return LOG_LEVEL_ERROR;
    if (strcasecmp(str, "all") == 0) return LOG_LEVEL_DEBUG;
    return LOG_LEVEL_INFO;
}

static u64 get_uptime_seconds(void) {
    return armTicksToNs(armGetSystemTick()) / 1000000000ULL;
}

void logger_init(AppState *state, const char *file_path) {
    if (!g_logger_initialized) {
        mutexInit(&g_logger_mutex);
        g_logger_initialized = true;
    }
    mutexLock(&g_logger_mutex);
    g_app_state = state;
    if (file_path && file_path[0] != '\0') {
        strncpy(g_log_file_path, file_path, sizeof(g_log_file_path) - 1);
        g_log_file_path[sizeof(g_log_file_path) - 1] = '\0';
    }
    mutexUnlock(&g_logger_mutex);
}

void logger_set_app_state(AppState *state) {
    if (!g_logger_initialized) {
        mutexInit(&g_logger_mutex);
        g_logger_initialized = true;
    }
    mutexLock(&g_logger_mutex);
    g_app_state = state;
    mutexUnlock(&g_logger_mutex);
}

static void rotate_log_file_if_needed(void) {
    FILE *f = fopen(g_log_file_path, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);

    // If file exceeds 256KB, rewrite with current ring buffer
    if (size > 256 * 1024) {
        FILE *out = fopen(g_log_file_path, "w");
        if (!out) return;
        size_t count = g_log_count < LOG_RING_CAPACITY ? g_log_count : LOG_RING_CAPACITY;
        size_t start = (g_log_head + LOG_RING_CAPACITY - count) % LOG_RING_CAPACITY;
        for (size_t i = 0; i < count; ++i) {
            size_t idx = (start + i) % LOG_RING_CAPACITY;
            LogEntry *e = &g_log_ring[idx];
            fprintf(out, "[%llus] [%s] [%s] %s\n", (unsigned long long) e->uptime_s, logger_level_name(e->level), e->module, e->message);
        }
        fclose(out);
    }
}

void logger_log_va(LogLevel level, const char *module, const char *fmt, va_list args) {
    if (!g_logger_initialized) {
        mutexInit(&g_logger_mutex);
        g_logger_initialized = true;
    }

    u64 uptime_s = get_uptime_seconds();
    char message[LOG_MSG_MAX];
    vsnprintf(message, sizeof(message), fmt, args);
    message[sizeof(message) - 1] = '\0';

    mutexLock(&g_logger_mutex);

    size_t idx = g_log_head;
    g_log_ring[idx].uptime_s = uptime_s;
    g_log_ring[idx].level = level;
    snprintf(g_log_ring[idx].module, sizeof(g_log_ring[idx].module), "%s", module ? module : "core");
    snprintf(g_log_ring[idx].message, sizeof(g_log_ring[idx].message), "%s", message);

    g_log_head = (g_log_head + 1) % LOG_RING_CAPACITY;
    if (g_log_count < LOG_RING_CAPACITY) {
        g_log_count++;
    }

    AppState *st = g_app_state;
    mutexUnlock(&g_logger_mutex);

    // Write to disk file
    FILE *f = fopen(g_log_file_path, "a");
    if (f) {
        fprintf(f, "[%llus] [%s] [%s] %s\n", (unsigned long long) uptime_s, logger_level_name(level), module ? module : "core", message);
        fclose(f);
    }

    g_log_write_counter++;
    if (g_log_write_counter % 50 == 0) {
        rotate_log_file_if_needed();
    }

    // Also update UI app state log display if available
    if (st) {
        app_state_push_log(st, "[%s] %s", module ? module : "core", message);
    }
}

void logger_log(LogLevel level, const char *module, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logger_log_va(level, module, fmt, args);
    va_end(args);
}

size_t logger_dump_logs_text(char *out_buf, size_t out_size, LogLevel min_level, int max_lines) {
    if (!out_buf || out_size == 0) return 0;
    out_buf[0] = '\0';

    if (!g_logger_initialized) return 0;
    mutexLock(&g_logger_mutex);

    size_t count = g_log_count < LOG_RING_CAPACITY ? g_log_count : LOG_RING_CAPACITY;
    if (max_lines > 0 && (size_t) max_lines < count) {
        count = (size_t) max_lines;
    }
    size_t start = (g_log_head + LOG_RING_CAPACITY - count) % LOG_RING_CAPACITY;
    size_t offset = 0;

    for (size_t i = 0; i < count && offset + 64 < out_size; ++i) {
        size_t idx = (start + i) % LOG_RING_CAPACITY;
        LogEntry *e = &g_log_ring[idx];
        if (e->level >= min_level) {
            int written = snprintf(out_buf + offset, out_size - offset,
                                   "[%llus] [%s] [%s] %s\n",
                                   (unsigned long long) e->uptime_s,
                                   logger_level_name(e->level),
                                   e->module,
                                   e->message);
            if (written > 0) {
                offset += (size_t) written;
            }
        }
    }

    mutexUnlock(&g_logger_mutex);
    return offset;
}

static void escape_json_string(const char *in, char *out, size_t out_size) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 2 < out_size; ++i) {
        char c = in[i];
        if (c == '"' || c == '\\') {
            out[j++] = '\\';
            out[j++] = c;
        } else if (c == '\n') {
            out[j++] = '\\';
            out[j++] = 'n';
        } else if (c == '\r') {
            out[j++] = '\\';
            out[j++] = 'r';
        } else if (c == '\t') {
            out[j++] = '\\';
            out[j++] = 't';
        } else if ((unsigned char) c >= 32) {
            out[j++] = c;
        }
    }
    out[j] = '\0';
}

size_t logger_dump_diagnostics_json(AppState *state, const AppConfig *config, char *out_buf, size_t out_size) {
    if (!out_buf || out_size == 0) return 0;

    const char *fw_ver = (state && state->cached_firmware_ver[0] != '\0') ? state->cached_firmware_ver : "Unknown";
    const char *ams_ver = (state && state->cached_atmosphere_ver[0] != '\0') ? state->cached_atmosphere_ver : "Unknown";
    const char *mac_str = (state && state->cached_mac[0] != '\0') ? state->cached_mac : "00:00:00:00:00:00";

    u32 rssi = 0;
    // Runs on the MQTT thread: these sessions are open while MQTT is connected.
    if (svc_mqtt_is_open(SvcId_Nifm)) {
        nifmGetInternetConnectionStatus(NULL, &rssi, NULL);
    }

    u32 battery_pct = 0;
    PsmChargerType charger = PsmChargerType_Unconnected;
    PsmBatteryChargeInfoFields batt_info = {0};
    if (svc_mqtt_is_open(SvcId_Psm)) {
        psmGetBatteryChargePercentage(&battery_pct);
        psmGetChargerType(&charger);
        psmGetBatteryChargeInfoFields(&batt_info);
    }

    s32 soc_temp = 0;
    if (svc_mqtt_is_open(SvcId_Ts)) {
        tsGetTemperature(TsLocation_Internal, &soc_temp);
    }

    s64 sd_free_mb = 0, sd_total_mb = 0;
    FsFileSystem sd_fs;
    if (R_SUCCEEDED(fsOpenSdCardFileSystem(&sd_fs))) {
        s64 bytes = 0;
        if (R_SUCCEEDED(fsFsGetFreeSpace(&sd_fs, "/", &bytes))) sd_free_mb = bytes / (1024 * 1024);
        if (R_SUCCEEDED(fsFsGetTotalSpace(&sd_fs, "/", &bytes))) sd_total_mb = bytes / (1024 * 1024);
        fsFsClose(&sd_fs);
    }

    s64 int_free_mb = 0, int_total_mb = 0;
    FsFileSystem user_fs;
    if (R_SUCCEEDED(fsOpenBisFileSystem(&user_fs, FsBisPartitionId_User, ""))) {
        s64 bytes = 0;
        if (R_SUCCEEDED(fsFsGetFreeSpace(&user_fs, "/", &bytes))) int_free_mb = bytes / (1024 * 1024);
        if (R_SUCCEEDED(fsFsGetTotalSpace(&user_fs, "/", &bytes))) int_total_mb = bytes / (1024 * 1024);
        fsFsClose(&user_fs);
    }

    u64 mem_used_mb = 0, mem_total_mb = 0;
    u64 val = 0;
    if (R_SUCCEEDED(svcGetInfo(&val, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0))) mem_total_mb = val / (1024 * 1024);
    if (R_SUCCEEDED(svcGetInfo(&val, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0))) mem_used_mb = val / (1024 * 1024);

    u64 uptime_s = get_uptime_seconds();

    size_t err_count = 0, warn_count = 0;
    if (g_logger_initialized) {
        mutexLock(&g_logger_mutex);
        for (size_t i = 0; i < g_log_count && i < LOG_RING_CAPACITY; ++i) {
            if (g_log_ring[i].level == LOG_LEVEL_ERROR) err_count++;
            else if (g_log_ring[i].level == LOG_LEVEL_WARN) warn_count++;
        }
        mutexUnlock(&g_logger_mutex);
    }

    const char *dev_name = config ? (config->device_name[0] ? config->device_name : "Nintendo Switch") : "Nintendo Switch";
    const char *mqtt_host = config ? config->mqtt_host : "";
    int mqtt_port = config ? config->mqtt_port : 1883;

    int written = snprintf(out_buf, out_size,
        "{"
        "\"status\":\"%s\","
        "\"errors\":%zu,"
        "\"warnings\":%zu,"
        "\"uptime_s\":%llu,"
        "\"device_name\":\"%s\","
        "\"firmware\":\"%s\","
        "\"atmosphere\":\"%s\","
        "\"mac\":\"%s\","
        "\"wifi_rssi\":%u,"
        "\"battery_pct\":%u,"
        "\"charging\":%s,"
        "\"charger_milli_volts\":%u,"
        "\"soc_temp_c\":%.1f,"
        "\"sd_free_mb\":%lld,"
        "\"sd_total_mb\":%lld,"
        "\"internal_free_mb\":%lld,"
        "\"internal_total_mb\":%lld,"
        "\"ram_used_mb\":%llu,"
        "\"ram_total_mb\":%llu,"
        "\"mqtt_host\":\"%s\","
        "\"mqtt_port\":%d,"
        "\"services\":{"
            "\"sm\":true,"
            "\"fs\":true,"
            "\"psm\":%s,"
            "\"spsm\":%s,"
            "\"spl\":%s,"
            "\"hid\":%s,"
            "\"lbl\":%s,"
            "\"audctl\":%s,"
            "\"pminfo\":%s,"
            "\"setsys\":%s,"
            "\"ts\":%s,"
            "\"nifm\":%s"
        "}"
        "}",
        (err_count == 0 ? (warn_count == 0 ? "healthy" : "warning") : "error"),
        err_count,
        warn_count,
        (unsigned long long) uptime_s,
        dev_name,
        fw_ver,
        ams_ver,
        mac_str,
        rssi,
        battery_pct,
        charger == PsmChargerType_Unconnected ? "false" : "true",
        batt_info.battery_charge_milli_voltage,
        (double) soc_temp / 1000.0,
        (long long) sd_free_mb,
        (long long) sd_total_mb,
        (long long) int_free_mb,
        (long long) int_total_mb,
        (unsigned long long) mem_used_mb,
        (unsigned long long) mem_total_mb,
        mqtt_host,
        mqtt_port,
        (state && state->svc_spsm_ready) ? "true" : "false",
        (state && state->svc_spsm_ready) ? "true" : "false",
        "true",
        (state && state->svc_hid_ready) ? "true" : "false",
        (state && state->svc_lbl_ready) ? "true" : "false",
        (state && state->svc_audctl_ready) ? "true" : "false",
        (state && state->svc_pminfo_ready) ? "true" : "false",
        (state && state->svc_setsys_ready) ? "true" : "false",
        (state && state->svc_ts_ready) ? "true" : "false",
        (state && state->svc_nifm_ready) ? "true" : "false"
    );

    return (written > 0 && (size_t) written < out_size) ? (size_t) written : 0;
}

size_t logger_dump_logs_json(AppState *state, const AppConfig *config, char *out_buf, size_t out_size, LogLevel min_level, int max_lines) {
    if (!out_buf || out_size < 128) return 0;

    size_t diag_len = logger_dump_diagnostics_json(state, config, out_buf, out_size);
    if (diag_len == 0 || diag_len >= out_size - 64) {
        snprintf(out_buf, out_size, "{\"status\":\"error\",\"logs\":[]}");
        return strlen(out_buf);
    }

    // Strip trailing '}' from diagnostics JSON so we can insert the "logs": [...] field
    if (out_buf[diag_len - 1] == '}') {
        out_buf[diag_len - 1] = '\0';
        diag_len--;
    }

    size_t offset = diag_len;
    int written = snprintf(out_buf + offset, out_size - offset, ",\"logs\":[");
    if (written > 0) offset += (size_t) written;

    if (g_logger_initialized) {
        mutexLock(&g_logger_mutex);
        size_t count = g_log_count < LOG_RING_CAPACITY ? g_log_count : LOG_RING_CAPACITY;
        if (max_lines > 0 && (size_t) max_lines < count) {
            count = (size_t) max_lines;
        }
        size_t start = (g_log_head + LOG_RING_CAPACITY - count) % LOG_RING_CAPACITY;
        bool first = true;

        for (size_t i = 0; i < count && offset + 256 < out_size; ++i) {
            size_t idx = (start + i) % LOG_RING_CAPACITY;
            LogEntry *e = &g_log_ring[idx];
            if (e->level >= min_level) {
                char esc_mod[32];
                char esc_msg[256];
                escape_json_string(e->module, esc_mod, sizeof(esc_mod));
                escape_json_string(e->message, esc_msg, sizeof(esc_msg));

                written = snprintf(out_buf + offset, out_size - offset,
                                   "%s{\"uptime\":%llu,\"level\":\"%s\",\"module\":\"%s\",\"msg\":\"%s\"}",
                                   first ? "" : ",",
                                   (unsigned long long) e->uptime_s,
                                   logger_level_name(e->level),
                                   esc_mod,
                                   esc_msg);
                if (written > 0) {
                    offset += (size_t) written;
                    first = false;
                }
            }
        }
        mutexUnlock(&g_logger_mutex);
    }

    if (offset + 3 < out_size) {
        out_buf[offset++] = ']';
        out_buf[offset++] = '}';
        out_buf[offset] = '\0';
    }

    return offset;
}
