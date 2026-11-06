#include "app_state.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "mqtt/mqtt_state.h"
#include "svc_session.h"

void app_state_init(AppState *state) {
    memset(state, 0, sizeof(*state));
    mutexInit(&state->lock);
    state->app_mode = AppMode_ForegroundApp;
    snprintf(state->status_line, sizeof(state->status_line), "Idle");
    snprintf(state->config_status, sizeof(state->config_status), "not loaded");
    snprintf(state->mqtt_last_error, sizeof(state->mqtt_last_error), "not connected");
    snprintf(state->game_status, sizeof(state->game_status), "not checked");
}

void app_state_refresh_device_info(AppState *state) {
    if (!state) return;

    mutexLock(&state->lock);

    // Invalidate cached AMS version if it accidentally matched the firmware version
    if (state->cached_firmware_ver[0] != '\0' &&
        strcmp(state->cached_atmosphere_ver, state->cached_firmware_ver) == 0) {
        state->cached_atmosphere_ver[0] = '\0';
    }

    // Always refresh firmware/AMS versions if either is missing or unknown
    if (state->cached_firmware_ver[0] == '\0' || strcmp(state->cached_firmware_ver, "Unknown") == 0 ||
        state->cached_atmosphere_ver[0] == '\0' || strcmp(state->cached_atmosphere_ver, "Unknown") == 0) {
        mqtt_get_system_versions(state, state->cached_firmware_ver, sizeof(state->cached_firmware_ver),
                                 state->cached_atmosphere_ver, sizeof(state->cached_atmosphere_ver));
    }

    // Use configured MAC address if present, otherwise derive from serial number
    if (state->cached_mac[0] == '\0') {
        if (state->config.mac_address[0] != '\0') {
            snprintf(state->cached_mac, sizeof(state->cached_mac), "%s", state->config.mac_address);
        } else {
            // setsys is opened only for this read (svc_session.h).
            if (R_SUCCEEDED(svc_open(SvcId_Setsys))) {
                SetSysSerialNumber serial = {0};
                if (R_SUCCEEDED(setsysGetSerialNumber(&serial)) && serial.number[0] != '\0') {
                    // Derive a stable 6-byte identifier from serial number chars
                    // Format: XX:XX:XX:XX:XX:XX using first 6 bytes of serial
                    unsigned char b[6] = {0};
                    for (int i = 0; i < 6 && serial.number[i] != '\0'; i++) {
                        b[i] = (unsigned char)serial.number[i];
                    }
                    snprintf(state->cached_mac, sizeof(state->cached_mac),
                             "%02x:%02x:%02x:%02x:%02x:%02x",
                             b[0], b[1], b[2], b[3], b[4], b[5]);
                }
                svc_close(SvcId_Setsys);
            }
        }
    }

    mutexUnlock(&state->lock);
}

static void write_line(char *dst, size_t dst_size, const char *fmt, va_list args) {
    vsnprintf(dst, dst_size, fmt, args);
    dst[dst_size - 1] = '\0';
}

void app_state_set_status(AppState *state, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    mutexLock(&state->lock);
    write_line(state->status_line, sizeof(state->status_line), fmt, args);
    mutexUnlock(&state->lock);
    va_end(args);
}

void app_state_push_log(AppState *state, const char *fmt, ...) {
    char line[SHA_LOG_LINE];
    va_list args;
    va_start(args, fmt);
    write_line(line, sizeof(line), fmt, args);
    va_end(args);

    mutexLock(&state->lock);
    for (int i = SHA_MAX_LOG - 1; i > 0; --i) {
        strncpy(state->log_lines[i], state->log_lines[i - 1], SHA_LOG_LINE - 1);
        state->log_lines[i][SHA_LOG_LINE - 1] = '\0';
    }
    strncpy(state->log_lines[0], line, SHA_LOG_LINE - 1);
    state->log_lines[0][SHA_LOG_LINE - 1] = '\0';
    if (state->log_count < SHA_MAX_LOG) {
        state->log_count++;
    }
    mutexUnlock(&state->lock);
}
