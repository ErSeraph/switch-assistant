#pragma once

#include "app_state.h"
#include <switch.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO  = 1,
    LOG_LEVEL_WARN  = 2,
    LOG_LEVEL_ERROR = 3,
} LogLevel;

#define LOG_RING_CAPACITY 150
#define LOG_MSG_MAX 160
#define LOG_MODULE_MAX 16

typedef struct {
    u64 uptime_s;
    LogLevel level;
    char module[LOG_MODULE_MAX];
    char message[LOG_MSG_MAX];
} LogEntry;

void logger_init(AppState *state, const char *file_path);
void logger_set_app_state(AppState *state);
void logger_log(LogLevel level, const char *module, const char *fmt, ...);
void logger_log_va(LogLevel level, const char *module, const char *fmt, va_list args);
// While suspended, log lines only go to the in-memory ring, never to the SD
// card: an SD access in flight while fs powers the card down for sleep can
// keep the console from completing the sleep transition. Resuming writes the
// lines logged meanwhile (up to the ring capacity) to the file.
void logger_set_file_suspended(bool suspended);

const char *logger_level_name(LogLevel level);
LogLevel logger_parse_level(const char *str);

size_t logger_dump_diagnostics_json(AppState *state, const AppConfig *config, char *out_buf, size_t out_size);
size_t logger_dump_logs_json(AppState *state, const AppConfig *config, char *out_buf, size_t out_size, LogLevel min_level, int max_lines);
size_t logger_dump_logs_text(char *out_buf, size_t out_size, LogLevel min_level, int max_lines);

#define LOG_D(mod, fmt, ...) logger_log(LOG_LEVEL_DEBUG, mod, fmt, ##__VA_ARGS__)
#define LOG_I(mod, fmt, ...) logger_log(LOG_LEVEL_INFO, mod, fmt, ##__VA_ARGS__)
#define LOG_W(mod, fmt, ...) logger_log(LOG_LEVEL_WARN, mod, fmt, ##__VA_ARGS__)
#define LOG_E(mod, fmt, ...) logger_log(LOG_LEVEL_ERROR, mod, fmt, ##__VA_ARGS__)
