#include "mqtt/mqtt_commands.h"
#include "mqtt/mqtt_notify.h"
#include "mqtt/mqtt_proto.h"
#include "mqtt/mqtt_topics.h"
#include "logger.h"
#include "title_cache.h"
#include "svc_session.h"


#include <switch.h>
#include <switch/services/pm.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void handle_launch_game_payload(AppState *state, int fd, const AppConfig *config, const char *payload) {
    (void) fd;
    (void) config;
    if (!payload || payload[0] == '\0') {
        return;
    }
    app_state_push_log(state, "Game launch request: %s", payload);

    if (strcasecmp(payload, "Home Menu") == 0 || strcasecmp(payload, "Home") == 0 ||
        strcasecmp(payload, "None") == 0 || strcasecmp(payload, "none") == 0 ||
        strcasecmp(payload, "off") == 0 || strcasecmp(payload, "idle") == 0) {
        app_state_push_log(state, "Game selection Home Menu/None/off requested");
        return;
    }

    u64 app_id = 0;
    if (title_cache_find_application_id_by_name(payload, &app_id)) {
        title_cache_launch_game(state, app_id);
    } else {
        app_state_push_log(state, "Game not found for: %s", payload);
    }
}

static void mqtt_publish_cmd_status(int fd, const AppConfig *config, const char *cmd, const char *status) {
    if (fd < 0 || !config || !cmd || !status) return;
    char stat_topic[192];
    char safe_id[80];
    mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(stat_topic, sizeof(stat_topic), "switch_ha/%s/command/status", safe_id);
    char payload[128];
    snprintf(payload, sizeof(payload), "{\"command\":\"%s\",\"status\":\"%s\"}", cmd, status);
    mqtt_send_publish(fd, stat_topic, payload, false);
}

typedef enum {
    PowerAction_Reboot,
    PowerAction_Shutdown,
    PowerAction_Sleep,
} PowerAction;

static const char *power_action_name(PowerAction action) {
    switch (action) {
        case PowerAction_Reboot: return "reboot";
        case PowerAction_Shutdown: return "shutdown";
        default: return "sleep";
    }
}

// spsm is opened only for the request: keeping a session open makes fatal/am
// abort on reboot (OutOfSessions, 0x615).
static Result request_power_off(bool reboot) {
    Result rc = svc_open(SvcId_Spsm);
    if (R_FAILED(rc)) {
        return rc;
    }
    rc = spsmShutdown(reboot);
    svc_close(SvcId_Spsm);
    return rc;
}

static Result request_sleep_spsm(void) {
    Result rc = svc_open(SvcId_Spsm);
    if (R_FAILED(rc)) {
        return rc;
    }
    // Command 1 on spsm IPowerStateInterface is EnterSleep
    rc = serviceDispatch(spsmGetServiceSession(), 1);
    svc_close(SvcId_Spsm);
    return rc;
}

// Power requests run on their own short-lived thread. spsm does not return
// until the power sequence moves on, and that sequence (psc) waits for the MQTT
// thread to publish the "off" states and close its socket; doing the request
// on the MQTT thread would block it until psc's timeout, with the socket open.
static Thread g_power_thread;
static bool g_power_thread_created = false;
static volatile bool g_power_thread_done = false;
static PowerAction g_power_action;
static AppState *g_power_state = NULL;

static void power_action_thread(void *arg) {
    (void) arg;
    PowerAction action = g_power_action;
    Result rc;
    if (action == PowerAction_Sleep) {
        rc = request_sleep_spsm();
        if (R_FAILED(rc)) {
            LOG_W("power", "sleep via spsm failed rc=0x%x, using svcSleepSystem", rc);
            // Privileged syscall svcSleepSystem (0x4D) fallback
            svcSleepSystem();
            rc = 0;
        }
    } else {
        rc = request_power_off(action == PowerAction_Reboot);
    }
    if (R_FAILED(rc)) {
        LOG_W("power", "%s failed rc=0x%x", power_action_name(action), rc);
        if (g_power_state) {
            app_state_push_log(g_power_state, "%s failed: spsm 0x%x", power_action_name(action), rc);
        }
    }
    g_power_thread_done = true;
}

static bool start_power_action(AppState *state, PowerAction action) {
    if (g_power_thread_created) {
        if (!g_power_thread_done) {
            return false;  // A request is still in progress.
        }
        threadWaitForExit(&g_power_thread);
        threadClose(&g_power_thread);
        g_power_thread_created = false;
    }
    g_power_action = action;
    g_power_state = state;
    g_power_thread_done = false;
    if (R_FAILED(threadCreate(&g_power_thread, power_action_thread, NULL, NULL, 0x4000, 0x2B, -2))) {
        return false;
    }
    if (R_FAILED(threadStart(&g_power_thread))) {
        threadClose(&g_power_thread);
        return false;
    }
    g_power_thread_created = true;
    return true;
}

static void handle_power_command(AppState *state, int fd, const AppConfig *config, PowerAction action, const char *source) {
    const char *name = power_action_name(action);
    LOG_I("mqtt", "%s %s accepted", source, name);
    app_state_push_log(state, "%s %s accepted", source, name);
    if (start_power_action(state, action)) {
        mqtt_publish_cmd_status(fd, config, name, "success");
    } else {
        LOG_W("mqtt", "%s %s failed to start", source, name);
        app_state_push_log(state, "%s %s failed to start", source, name);
        mqtt_publish_cmd_status(fd, config, name, "failed");
    }
}

static void handle_power_off_command(AppState *state, int fd, const AppConfig *config, bool reboot, const char *source) {
    handle_power_command(state, fd, config, reboot ? PowerAction_Reboot : PowerAction_Shutdown, source);
}

static void handle_remote_command(AppState *state, int fd, const AppConfig *config, const char *cmd) {
    if (!cmd || cmd[0] == '\0') {
        return;
    }
    app_state_push_log(state, "Remote command: %s", cmd);

    if (strcasecmp(cmd, "reboot") == 0 || strcasecmp(cmd, "restart") == 0) {
        handle_power_off_command(state, fd, config, true, "Remote");
    } else if (strcasecmp(cmd, "shutdown") == 0 || strcasecmp(cmd, "poweroff") == 0 || strcasecmp(cmd, "turn_off") == 0 || strcasecmp(cmd, "off") == 0) {
        handle_power_off_command(state, fd, config, false, "Remote");
    } else if (strcasecmp(cmd, "sleep") == 0 || strcasecmp(cmd, "standby") == 0) {
        app_state_push_log(state, "Remote sleep requested");
        if (state->app_mode != AppMode_Sysmodule) {
            Result rc = appletRequestToSleep();
            if (R_FAILED(rc)) {
                rc = appletStartSleepSequence(true);
            }
            LOG_I("mqtt", "Remote sleep rc=0x%x (mode=%d)", rc, state->app_mode);
            app_state_push_log(state, "Remote sleep rc=0x%x", rc);
            mqtt_publish_cmd_status(fd, config, "sleep", R_SUCCEEDED(rc) ? "success" : "failed");
        } else {
            // In sysmodule mode: spsm (or svcSleepSystem) on the power thread
            handle_power_command(state, fd, config, PowerAction_Sleep, "Remote");
        }
    } else if (strcasecmp(cmd, "close_game") == 0 || strcasecmp(cmd, "close") == 0 || strcasecmp(cmd, "exit_game") == 0) {
        app_state_push_log(state, "Remote close game requested");
        bool terminated = false;
        // 1. Try pmshellTerminateProgram / pmshellTerminateProcess via pm:shell
        bool pmshell_ready = false;
        if (R_SUCCEEDED(pmshellInitialize())) {
            pmshell_ready = true;
        }

        if (state->svc_pminfo_ready) {
            u64 process_ids[96];
            s32 process_count = 0;
            if (R_SUCCEEDED(svcGetProcessList(&process_count, process_ids, 96))) {
                if (process_count > 96) process_count = 96;
                for (s32 i = 0; i < process_count; ++i) {
                    u64 prog_id = 0;
                    if (R_SUCCEEDED(pminfoGetProgramId(&prog_id, process_ids[i])) && prog_id != 0) {
                        char name[128];
                        if (title_cache_lookup(prog_id, name, sizeof(name))) {
                            LOG_I("mqtt", "Terminating running game pid=%llu prog=0x%016llx (%s)",
                                  (unsigned long long)process_ids[i], (unsigned long long)prog_id, name);
                            Result rc_term = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
                            if (pmshell_ready) {
                                rc_term = pmshellTerminateProgram(prog_id);
                                if (R_FAILED(rc_term)) {
                                    rc_term = pmshellTerminateProcess(process_ids[i]);
                                }
                            }
                            if (R_FAILED(rc_term)) {
                                rc_term = svcTerminateProcess(process_ids[i]);
                            }
                            LOG_I("mqtt", "Termination rc=0x%x for '%s'", rc_term, name);
                            app_state_push_log(state, "Close game '%s' rc=0x%x", name, rc_term);
                            terminated = true;
                            break;
                        }
                    }
                }
            }
        }
        if (pmshell_ready) {
            pmshellExit();
        }
        if (!terminated) {
            if (state->app_mode != AppMode_Sysmodule) {
                libappletRequestHomeMenu();
            }
            app_state_push_log(state, "Close game: no running game found");
            mqtt_publish_cmd_status(fd, config, "close_game", "no_game_running");
        } else {
            mqtt_publish_cmd_status(fd, config, "close_game", "success");
        }
        mutexLock(&state->lock);
        state->sensor_publish_requested = true;
        mutexUnlock(&state->lock);
    } else if (strcasecmp(cmd, "volume_up") == 0) {
        if (state->svc_audctl_ready) {
            AudioTarget audio_target = AudioTarget_Invalid;
            if (R_SUCCEEDED(audctlGetActiveOutputTarget(&audio_target)) || R_SUCCEEDED(audctlGetDefaultTarget(&audio_target))) {
                s32 vol = 0, min_v = 0, max_v = 0;
                if (R_SUCCEEDED(audctlGetTargetVolume(&vol, audio_target)) &&
                    R_SUCCEEDED(audctlGetTargetVolumeMin(&min_v)) &&
                    R_SUCCEEDED(audctlGetTargetVolumeMax(&max_v))) {
                    s32 step = (max_v - min_v) / 20;
                    if (step < 1) step = 1;
                    vol += step;
                    if (vol > max_v) vol = max_v;
                    audctlSetTargetVolume(audio_target, vol);
                    mutexLock(&state->lock);
                    state->sensor_publish_requested = true;
                    mutexUnlock(&state->lock);
                }
            }
        }
    } else if (strcasecmp(cmd, "volume_down") == 0) {
        if (state->svc_audctl_ready) {
            AudioTarget audio_target = AudioTarget_Invalid;
            if (R_SUCCEEDED(audctlGetActiveOutputTarget(&audio_target)) || R_SUCCEEDED(audctlGetDefaultTarget(&audio_target))) {
                s32 vol = 0, min_v = 0, max_v = 0;
                if (R_SUCCEEDED(audctlGetTargetVolume(&vol, audio_target)) &&
                    R_SUCCEEDED(audctlGetTargetVolumeMin(&min_v)) &&
                    R_SUCCEEDED(audctlGetTargetVolumeMax(&max_v))) {
                    s32 step = (max_v - min_v) / 20;
                    if (step < 1) step = 1;
                    vol += step;
                    if (vol < min_v) vol = min_v;
                    audctlSetTargetVolume(audio_target, vol);
                    mutexLock(&state->lock);
                    state->sensor_publish_requested = true;
                    mutexUnlock(&state->lock);
                }
            }
        }
    } else if (strcasecmp(cmd, "volume_mute") == 0 || strcasecmp(cmd, "mute") == 0) {
        if (state->svc_audctl_ready) {
            AudioTarget audio_target = AudioTarget_Invalid;
            if (R_SUCCEEDED(audctlGetActiveOutputTarget(&audio_target)) || R_SUCCEEDED(audctlGetDefaultTarget(&audio_target))) {
                s32 min_v = 0;
                if (R_SUCCEEDED(audctlGetTargetVolumeMin(&min_v))) {
                    audctlSetTargetVolume(audio_target, min_v);
                    mutexLock(&state->lock);
                    state->sensor_publish_requested = true;
                    mutexUnlock(&state->lock);
                }
            }
        }
    } else if (strcasecmp(cmd, "brightness_up") == 0) {
        if (R_SUCCEEDED(svc_open(SvcId_Lbl))) {
            float b = 0.0f;
            if (R_SUCCEEDED(lblGetCurrentBrightnessSetting(&b))) {
                b += 0.10f;
                if (b > 1.0f) b = 1.0f;
                lblSetCurrentBrightnessSetting(b);
                lblApplyCurrentBrightnessSettingToBacklight();
                mutexLock(&state->lock);
                state->sensor_publish_requested = true;
                mutexUnlock(&state->lock);
            }
            svc_close(SvcId_Lbl);
        }
    } else if (strcasecmp(cmd, "brightness_down") == 0) {
        if (R_SUCCEEDED(svc_open(SvcId_Lbl))) {
            float b = 0.0f;
            if (R_SUCCEEDED(lblGetCurrentBrightnessSetting(&b))) {
                b -= 0.10f;
                if (b < 0.0f) b = 0.0f;
                lblSetCurrentBrightnessSetting(b);
                lblApplyCurrentBrightnessSettingToBacklight();
                mutexLock(&state->lock);
                state->sensor_publish_requested = true;
                mutexUnlock(&state->lock);
            }
            svc_close(SvcId_Lbl);
        }
    } else if (strcasecmp(cmd, "screen_on") == 0) {
        if (R_SUCCEEDED(svc_open(SvcId_Lbl))) {
            lblSwitchBacklightOn(0);
            svc_close(SvcId_Lbl);
            mutexLock(&state->lock);
            state->sensor_publish_requested = true;
            mutexUnlock(&state->lock);
        }
    } else if (strcasecmp(cmd, "screen_off") == 0) {
        if (R_SUCCEEDED(svc_open(SvcId_Lbl))) {
            lblSwitchBacklightOff(0);
            svc_close(SvcId_Lbl);
            mutexLock(&state->lock);
            state->sensor_publish_requested = true;
            mutexUnlock(&state->lock);
        }
    }
}

static void handle_remote_send_command_payload(AppState *state, int fd, const AppConfig *config, const char *payload) {
    if (!payload || payload[0] == '\0') {
        return;
    }

    if (payload[0] == '{') {
        char cmd[64];
        if (mqtt_json_extract_string(payload, "command", cmd, sizeof(cmd))) {
            handle_remote_command(state, fd, config, cmd);
            return;
        }
    }

    const char *ptr = payload;
    while (*ptr) {
        while (*ptr == ' ' || *ptr == '\t' || *ptr == '\r' || *ptr == '\n' || *ptr == ',' || *ptr == '"' || *ptr == '[' || *ptr == ']') {
            ptr++;
        }
        if (!*ptr) break;
        const char *start = ptr;
        while (*ptr && *ptr != ' ' && *ptr != '\t' && *ptr != '\r' && *ptr != '\n' && *ptr != ',' && *ptr != '"' && *ptr != ']' && *ptr != '[') {
            ptr++;
        }
        size_t len = (size_t)(ptr - start);
        if (len > 0 && len < 64) {
            char cmd[64];
            memcpy(cmd, start, len);
            cmd[len] = '\0';
            handle_remote_command(state, fd, config, cmd);
        }
    }
}

static void handle_remote_set_payload(AppState *state, int fd, const AppConfig *config, const char *payload) {
    if (!payload || payload[0] == '\0') {
        return;
    }
    app_state_push_log(state, "Remote set: %s", payload);
    if (strcasecmp(payload, "OFF") == 0 || strcasecmp(payload, "false") == 0 || strcasecmp(payload, "0") == 0) {
        app_state_push_log(state, "Remote set OFF -> shutdown");
        handle_power_off_command(state, fd, config, false, "Remote set OFF");
    }
}

static void handle_media_player_cmd(AppState *state, int fd, const AppConfig *config, const char *payload) {
    if (!payload || payload[0] == '\0') {
        return;
    }
    app_state_push_log(state, "Media player cmd: %s", payload);
    if (strcasecmp(payload, "volume_up") == 0 || strcasecmp(payload, "volume_down") == 0 ||
        strcasecmp(payload, "mute") == 0 || strcasecmp(payload, "volume_mute") == 0 ||
        strcasecmp(payload, "turn_off") == 0 || strcasecmp(payload, "off") == 0) {
        handle_remote_command(state, fd, config, payload);
    }
}

static void handle_media_player_volume_set(AppState *state, int fd, const AppConfig *config, const char *payload) {
    (void) fd;
    (void) config;
    if (!payload || payload[0] == '\0') {
        return;
    }
    double frac = atof(payload);
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    if (state->svc_audctl_ready) {
        AudioTarget audio_target = AudioTarget_Invalid;
        if (R_SUCCEEDED(audctlGetActiveOutputTarget(&audio_target)) || R_SUCCEEDED(audctlGetDefaultTarget(&audio_target))) {
            s32 min_v = 0, max_v = 0;
            if (R_SUCCEEDED(audctlGetTargetVolumeMin(&min_v)) && R_SUCCEEDED(audctlGetTargetVolumeMax(&max_v)) && max_v > min_v) {
                s32 vol = min_v + (s32)(frac * (double)(max_v - min_v));
                if (vol < min_v) vol = min_v;
                if (vol > max_v) vol = max_v;
                audctlSetTargetVolume(audio_target, vol);
                mutexLock(&state->lock);
                state->sensor_publish_requested = true;
                mutexUnlock(&state->lock);
            }
        }
    }
}

static void handle_atmosphere_update_command(AppState *state, int fd, const AppConfig *config, const char *payload) {
    (void) fd;
    (void) config;
    app_state_push_log(state, "Atmosphere update requested via HA: %s", payload ? payload : "install");

    FILE *f = fopen("sdmc:/switch/switch-ha/atmosphere_update.ini", "w");
    if (f) {
        fprintf(f, "requested_version=%s\nstatus=requested\n", payload ? payload : "latest");
        fclose(f);
    }

    mqtt_write_notification_files("popup", "Atmosphère Update", "Update request recorded. Check app or updater to apply.");
    app_state_push_log(state, "Atmosphere update command recorded");
}

static void handle_firmware_update_command(AppState *state, int fd, const AppConfig *config, const char *payload) {
    (void) fd;
    (void) config;
    app_state_push_log(state, "Firmware update requested via HA: %s", payload ? payload : "install");

    // Automatically clean existing /firmware directory on SD card
    mqtt_prepare_firmware_directory();

    FILE *f = fopen("sdmc:/switch/switch-ha/firmware_update.ini", "w");
    if (f) {
        fprintf(f, "requested_version=%s\nstatus=prepared\n", payload ? payload : "latest");
        fclose(f);
    }

    u64 daybreak_id = 0;
    bool launched = false;
    if (title_cache_find_application_id_by_name("Daybreak", &daybreak_id) ||
        title_cache_find_application_id_by_name("hbmenu", &daybreak_id) ||
        title_cache_find_application_id_by_name("Homebrew Menu", &daybreak_id)) {
        launched = title_cache_launch_game(state, daybreak_id);
    }

    if (!launched) {
        // Attempt launching via Album / Photo Viewer takeover (0x010000000000100D)
        Result rc = appletRequestLaunchApplication(0x010000000000100DULL, NULL);
        if (R_FAILED(rc)) {
            if (R_SUCCEEDED(appletInitialize())) {
                rc = appletRequestLaunchApplication(0x010000000000100DULL, NULL);
                appletExit();
            }
        }
        launched = R_SUCCEEDED(rc);
    }

    if (launched) {
        mqtt_write_notification_files("popup", "Firmware Update", "sdmc:/firmware ready. Launching Daybreak...");
    } else {
        mqtt_write_notification_files("popup", "Firmware Update", "sdmc:/firmware ready. Open Daybreak in Album/HBMenu.");
    }
    app_state_push_log(state, "Firmware directory /firmware cleaned and ready for Daybreak (launched=%d)", launched);
}

static const char *case_str_find(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    if (needle[0] == '\0') return haystack;
    size_t needle_len = strlen(needle);
    for (size_t i = 0; haystack[i]; ++i) {
        if (strncasecmp(haystack + i, needle, needle_len) == 0) {
            return haystack + i;
        }
    }
    return NULL;
}

static void handle_get_logs_command(AppState *state, int fd, const AppConfig *config, const char *payload) {
    LogLevel min_level = LOG_LEVEL_INFO;
    int max_lines = 100;

    if (payload && payload[0] == '{') {
        char lvl_str[32];
        if (mqtt_json_extract_string(payload, "level", lvl_str, sizeof(lvl_str)) ||
            mqtt_json_extract_string(payload, "filter", lvl_str, sizeof(lvl_str))) {
            min_level = logger_parse_level(lvl_str);
        }
        int lines_req = 0;
        if (mqtt_json_extract_int(payload, "lines", &lines_req) ||
            mqtt_json_extract_int(payload, "max_lines", &lines_req) ||
            mqtt_json_extract_int(payload, "count", &lines_req)) {
            if (lines_req > 0 && lines_req <= 200) max_lines = lines_req;
        }
    } else if (payload && payload[0] != '\0') {
        if (case_str_find(payload, "debug")) min_level = LOG_LEVEL_DEBUG;
        else if (case_str_find(payload, "warn")) min_level = LOG_LEVEL_WARN;
        else if (case_str_find(payload, "err")) min_level = LOG_LEVEL_ERROR;
        else if (case_str_find(payload, "all")) min_level = LOG_LEVEL_DEBUG;
    }

    size_t buf_size = 32768;
    char *buf = malloc(buf_size);
    if (!buf) {
        LOG_E("mqtt", "Failed to allocate buffer for logs export");
        return;
    }

    size_t len = logger_dump_logs_json(state, config, buf, buf_size, min_level, max_lines);
    if (len > 0) {
        char resp_topic[192];
        mqtt_logs_response_topic(config, resp_topic, sizeof(resp_topic));
        mqtt_send_publish(fd, resp_topic, buf, false);

        char diag_attr_topic[192];
        mqtt_diagnostics_attr_topic(config, diag_attr_topic, sizeof(diag_attr_topic));
        mqtt_send_publish(fd, diag_attr_topic, buf, true);

        char diag_state_topic[192];
        mqtt_diagnostics_state_topic(config, diag_state_topic, sizeof(diag_state_topic));
        mqtt_send_publish(fd, diag_state_topic, "healthy", true);

        LOG_I("mqtt", "Exported %zu bytes of logs & diagnostics (level=%s, max_lines=%d)", len, logger_level_name(min_level), max_lines);
    }
    free(buf);
}

static void handle_command_payload(AppState *state, int fd, const AppConfig *config, const char *payload) {
    if (!payload || payload[0] == '\0') {
        return;
    }

    LOG_I("mqtt", "MQTT command received: %s", payload);
    app_state_push_log(state, "MQTT command: %s", payload);

    if (strcasecmp(payload, "get_logs") == 0 || strcasecmp(payload, "logs") == 0 ||
        strcasecmp(payload, "diagnostics") == 0 || strncasecmp(payload, "get_logs ", 9) == 0 ||
        (payload[0] == '{' && (strstr(payload, "get_logs") || strstr(payload, "diagnostics")))) {
        handle_get_logs_command(state, fd, config, payload);
    } else if (strcasecmp(payload, "reboot") == 0) {
        handle_power_off_command(state, fd, config, true, "Command");
    } else if (strcasecmp(payload, "shutdown") == 0 || strcasecmp(payload, "poweroff") == 0) {
        handle_power_off_command(state, fd, config, false, "Command");
    } else if (strcasecmp(payload, "sleep") == 0 || strcasecmp(payload, "standby") == 0) {
        handle_remote_command(state, fd, config, "sleep");
    } else if (strcasecmp(payload, "close_game") == 0 || strcasecmp(payload, "close") == 0 || strcasecmp(payload, "exit_game") == 0) {
        handle_remote_command(state, fd, config, "close_game");
    } else if (strcasecmp(payload, "launch_daybreak") == 0 || strcasecmp(payload, "daybreak") == 0) {
        handle_firmware_update_command(state, fd, config, "daybreak");
    } else if (strncasecmp(payload, "launch_game ", 12) == 0) {
        handle_launch_game_payload(state, fd, config, payload + 12);
    } else if (strncasecmp(payload, "launch ", 7) == 0) {
        handle_launch_game_payload(state, fd, config, payload + 7);
    } else if (strncasecmp(payload, "select_source ", 14) == 0) {
        handle_launch_game_payload(state, fd, config, payload + 14);
    } else {
        handle_remote_command(state, fd, config, payload);
    }
}

static void handle_notification_payload(AppState *state, int fd, const AppConfig *config, const char *topic, const char *payload) {
    char popup_topic[192];
    char status_topic[192];
    char mode[16];
    char title[96];
    char message[NOTIFY_TEXT_MAX];

    (void) topic;
    mqtt_notification_topic(config, "popup", popup_topic, sizeof(popup_topic));
    mqtt_notification_status_topic(config, status_topic, sizeof(status_topic));

    snprintf(mode, sizeof(mode), "popup");
    snprintf(title, sizeof(title), "Home Assistant");
    mqtt_copy_text(message, sizeof(message), payload);

    if (payload[0] == '{') {
        mqtt_json_extract_string(payload, "title", title, sizeof(title));
        mqtt_json_extract_string(payload, "message", message, sizeof(message));
    } else {
        const char *line = strchr(payload, '\n');
        if (line && line > payload && (size_t) (line - payload) < sizeof(title)) {
            memcpy(title, payload, (size_t) (line - payload));
            title[line - payload] = '\0';
            mqtt_copy_text(message, sizeof(message), line + 1);
        }
    }

    mqtt_trim_text(mode);
    mqtt_trim_text(title);
    mqtt_trim_text(message);
    if (title[0] == '\0') {
        snprintf(title, sizeof(title), "Home Assistant");
    }
    if (message[0] == '\0') {
        mqtt_send_publish(fd, status_topic, "empty", false);
        return;
    }

    bool ok = mqtt_write_notification_files(mode, title, message);
    mqtt_send_publish(fd, status_topic, ok ? "queued" : "queue_failed", false);
    app_state_push_log(state, "Notify %s: %.48s", mode, message);
}

void mqtt_process_overlay_commands(AppState *state, int fd, const AppConfig *config) {
    FILE *file = fopen("sdmc:/switch/switch-ha/overlay-command.ini", "r");
    if (!file) {
        return;
    }

    char action[64] = "toggle";
    char domain[64] = "";
    char service[64] = "";
    char entity_id[128] = "";
    char name[128] = "";
    char line[256];

    while (fgets(line, sizeof(line), file)) {
        mqtt_trim_text(line);
        char *sep = strchr(line, '=');
        if (!sep) continue;
        *sep = '\0';
        const char *key = line;
        const char *val = sep + 1;

        if (strcasecmp(key, "action") == 0) {
            snprintf(action, sizeof(action), "%s", val);
        } else if (strcasecmp(key, "domain") == 0) {
            snprintf(domain, sizeof(domain), "%s", val);
        } else if (strcasecmp(key, "service") == 0) {
            snprintf(service, sizeof(service), "%s", val);
        } else if (strcasecmp(key, "entity_id") == 0) {
            snprintf(entity_id, sizeof(entity_id), "%s", val);
        } else if (strcasecmp(key, "name") == 0) {
            snprintf(name, sizeof(name), "%s", val);
        }
    }
    fclose(file);
    remove("sdmc:/switch/switch-ha/overlay-command.ini");

    if (domain[0] == '\0' && entity_id[0] != '\0') {
        const char *dot = strchr(entity_id, '.');
        if (dot && (size_t)(dot - entity_id) < sizeof(domain)) {
            memcpy(domain, entity_id, dot - entity_id);
            domain[dot - entity_id] = '\0';
        }
    }
    if (service[0] == '\0') {
        snprintf(service, sizeof(service), "%s", action);
    }

    char event_top[192];
    char service_top[192];
    mqtt_overlay_event_topic(config, event_top, sizeof(event_top));
    mqtt_call_service_topic(config, service_top, sizeof(service_top));

    char payload_event[256];
    snprintf(payload_event, sizeof(payload_event),
             "{\"event_type\":\"%s\",\"entity_id\":\"%s\",\"domain\":\"%s\",\"service\":\"%s\"}",
             action, entity_id, domain, service);
    mqtt_send_publish(fd, event_top, payload_event, false);

    char payload_service[256];
    snprintf(payload_service, sizeof(payload_service),
             "{\"domain\":\"%s\",\"service\":\"%s\",\"entity_id\":\"%s\"}",
             domain[0] ? domain : "homeassistant", service, entity_id);
    mqtt_send_publish(fd, service_top, payload_service, false);

    char toast[128];
    const char *display_target = name[0] ? name : (entity_id[0] ? entity_id : action);
    snprintf(toast, sizeof(toast), "HA Action: %s (%s)", display_target, service);
    mqtt_write_notification_files("popup", "HA Control", toast);
    app_state_push_log(state, "Overlay command executed: %s", toast);
}

void mqtt_handle_publish(AppState *state, int fd, const AppConfig *config, const unsigned char *buffer, int size) {
    int remaining = 0;
    int remaining_bytes = 0;
    if (!mqtt_read_remaining_length(buffer, size, &remaining, &remaining_bytes)) {
        return;
    }

    int pos = 1 + remaining_bytes;
    if (pos + 2 > size) {
        return;
    }

    int topic_len = ((int) buffer[pos] << 8) | buffer[pos + 1];
    pos += 2;
    if (topic_len <= 0 || pos + topic_len > size) {
        return;
    }

    char topic[192];
    int copy_topic_len = topic_len;
    if (copy_topic_len >= (int) sizeof(topic)) {
        copy_topic_len = (int) sizeof(topic) - 1;
    }
    memcpy(topic, buffer + pos, (size_t) copy_topic_len);
    topic[copy_topic_len] = '\0';
    pos += topic_len;

    int qos = (buffer[0] & 0x06) >> 1;
    if (qos == 1) {
        if (pos + 2 > size) {
            return;
        }
        unsigned char puback[4] = {0x40, 0x02, buffer[pos], buffer[pos + 1]};
        mqtt_send_packet(fd, puback, sizeof(puback));
        pos += 2;
    } else if (qos > 1) {
        pos += 2;
    }
    if (pos > size) {
        return;
    }

    int packet_len = 1 + remaining_bytes + remaining;
    if (packet_len > size) {
        packet_len = size;
    }
    int payload_len = packet_len - pos;
    if (payload_len < 0) {
        return;
    }
    if (payload_len >= NOTIFY_TEXT_MAX) {
        payload_len = NOTIFY_TEXT_MAX - 1;
    }

    char payload[NOTIFY_TEXT_MAX];
    memcpy(payload, buffer + pos, (size_t) payload_len);
    payload[payload_len] = '\0';
    mqtt_trim_text(payload);

    char command_topic[SHA_MAX_TOPIC + 32];
    char dev_cmd_topic[192];
    char popup_topic[192];
    char send_cmd_topic[192];
    char cmd_set_topic[192];
    char mp_cmd_topic[192];
    char mp_vol_cmd_topic[192];
    char mp_src_cmd_topic[192];
    char rem_act_cmd_topic[192];
    char sel_game_cmd_topic[192];
    char sel_game_2p_cmd_topic[192];
    char sel_game_4p_cmd_topic[192];
    char sel_game_8p_cmd_topic[192];
    char update_cmd_topic[192];
    char ams_update_cmd_topic[192];
    char br_cmd_topic[192];
    char bl_cmd_topic[192];
    char logs_cmd_topic[192];

    snprintf(command_topic, sizeof(command_topic), "%s/command", config->mqtt_topic_prefix);
    mqtt_command_topic(config, dev_cmd_topic, sizeof(dev_cmd_topic));
    mqtt_notification_topic(config, "popup", popup_topic, sizeof(popup_topic));
    mqtt_remote_send_command_topic(config, send_cmd_topic, sizeof(send_cmd_topic));
    mqtt_remote_set_topic(config, cmd_set_topic, sizeof(cmd_set_topic));
    mqtt_media_player_command_topic(config, mp_cmd_topic, sizeof(mp_cmd_topic));
    mqtt_media_player_volume_cmd_topic(config, mp_vol_cmd_topic, sizeof(mp_vol_cmd_topic));
    mqtt_media_player_source_cmd_topic(config, mp_src_cmd_topic, sizeof(mp_src_cmd_topic));
    mqtt_remote_activity_cmd_topic(config, rem_act_cmd_topic, sizeof(rem_act_cmd_topic));
    mqtt_select_game_cmd_topic(config, sel_game_cmd_topic, sizeof(sel_game_cmd_topic));
    mqtt_select_game_2p_cmd_topic(config, sel_game_2p_cmd_topic, sizeof(sel_game_2p_cmd_topic));
    mqtt_select_game_4p_cmd_topic(config, sel_game_4p_cmd_topic, sizeof(sel_game_4p_cmd_topic));
    mqtt_select_game_8p_cmd_topic(config, sel_game_8p_cmd_topic, sizeof(sel_game_8p_cmd_topic));
    mqtt_update_command_topic(config, update_cmd_topic, sizeof(update_cmd_topic));
    mqtt_atmosphere_update_command_topic(config, ams_update_cmd_topic, sizeof(ams_update_cmd_topic));
    mqtt_brightness_cmd_topic(config, br_cmd_topic, sizeof(br_cmd_topic));
    mqtt_backlight_cmd_topic(config, bl_cmd_topic, sizeof(bl_cmd_topic));
    mqtt_logs_command_topic(config, logs_cmd_topic, sizeof(logs_cmd_topic));

    LOG_I("mqtt", "RX publish topic='%s' payload='%s'", topic, payload);
    app_state_push_log(state, "MQTT RX: %s -> %s", topic, payload);

    bool is_command = (strcmp(topic, command_topic) == 0 || strcmp(topic, dev_cmd_topic) == 0);
    if (!is_command) {
        char safe_id[80];
        mqtt_sanitize_id(mqtt_client_id_or_device(config), safe_id, sizeof(safe_id));
        size_t tlen = strlen(topic);
        if (tlen >= 8 && strcmp(topic + tlen - 8, "/command") == 0) {
            if (strstr(topic, safe_id) != NULL) {
                is_command = true;
            }
        }
    }

    if (is_command) {
        handle_command_payload(state, fd, config, payload);
    } else if (strcmp(topic, logs_cmd_topic) == 0) {
        handle_get_logs_command(state, fd, config, payload);
    } else if (strcmp(topic, popup_topic) == 0) {
        handle_notification_payload(state, fd, config, topic, payload);
    } else if (strcmp(topic, send_cmd_topic) == 0) {
        handle_remote_send_command_payload(state, fd, config, payload);
    } else if (strcmp(topic, cmd_set_topic) == 0) {
        handle_remote_set_payload(state, fd, config, payload);
    } else if (strcmp(topic, mp_cmd_topic) == 0) {
        handle_media_player_cmd(state, fd, config, payload);
    } else if (strcmp(topic, mp_vol_cmd_topic) == 0) {
        handle_media_player_volume_set(state, fd, config, payload);
    } else if (strcmp(topic, mp_src_cmd_topic) == 0 ||
               strcmp(topic, rem_act_cmd_topic) == 0 ||
               strcmp(topic, sel_game_cmd_topic) == 0 ||
               strcmp(topic, sel_game_2p_cmd_topic) == 0 ||
               strcmp(topic, sel_game_4p_cmd_topic) == 0 ||
               strcmp(topic, sel_game_8p_cmd_topic) == 0) {
        handle_launch_game_payload(state, fd, config, payload);
    } else if (strcmp(topic, update_cmd_topic) == 0) {
        handle_firmware_update_command(state, fd, config, payload);
    } else if (strcmp(topic, ams_update_cmd_topic) == 0) {
        handle_atmosphere_update_command(state, fd, config, payload);
    } else if (strcmp(topic, br_cmd_topic) == 0) {
        if (R_SUCCEEDED(svc_open(SvcId_Lbl))) {
            float val = (float) atof(payload);
            if (val < 0.0f) val = 0.0f;
            if (val > 100.0f) val = 100.0f;
            Result rc = lblSetCurrentBrightnessSetting(val / 100.0f);
            Result rc_apply = lblApplyCurrentBrightnessSettingToBacklight();
            svc_close(SvcId_Lbl);
            LOG_I("mqtt", "Set brightness to %.1f%% (rc=0x%x, apply_rc=0x%x)", val, rc, rc_apply);
            app_state_push_log(state, "Set brightness to %.0f%% (rc=0x%x)", val, rc);
            char val_str[16];
            snprintf(val_str, sizeof(val_str), "%.0f", val);
            char br_stat_top[192];
            mqtt_brightness_state_topic(config, br_stat_top, sizeof(br_stat_top));
            mqtt_send_publish(fd, br_stat_top, val_str, true);
            mutexLock(&state->lock);
            state->sensor_publish_requested = true;
            mutexUnlock(&state->lock);
        } else {
            LOG_W("mqtt", "Set brightness failed: lbl unavailable");
            app_state_push_log(state, "Set brightness failed: lbl unavailable");
        }
    } else if (strcmp(topic, bl_cmd_topic) == 0) {
        if (R_SUCCEEDED(svc_open(SvcId_Lbl))) {
            bool on = (strcasecmp(payload, "ON") == 0 || strcasecmp(payload, "true") == 0 || strcasecmp(payload, "1") == 0);
            Result rc = on ? lblSwitchBacklightOn(0) : lblSwitchBacklightOff(0);
            svc_close(SvcId_Lbl);
            LOG_I("mqtt", "Switch backlight %s (rc=0x%x)", on ? "ON" : "OFF", rc);
            app_state_push_log(state, "Switch backlight %s (rc=0x%x)", on ? "ON" : "OFF", rc);
            char bl_stat_top[192];
            mqtt_backlight_state_topic(config, bl_stat_top, sizeof(bl_stat_top));
            mqtt_send_publish(fd, bl_stat_top, on ? "ON" : "OFF", true);
            mutexLock(&state->lock);
            state->sensor_publish_requested = true;
            mutexUnlock(&state->lock);
        } else {
            LOG_W("mqtt", "Switch backlight failed: lbl unavailable");
            app_state_push_log(state, "Switch backlight failed: lbl unavailable");
        }
    }
}
