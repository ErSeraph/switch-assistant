#include "mqtt/mqtt_state.h"
#include "mqtt/mqtt_proto.h"
#include "mqtt/mqtt_topics.h"
#include "screen_stream.h"
#include "svc_session.h"
#include "title_cache.h"

#include <switch.h>
#include <switch/services/applet.h>
#include <switch/services/ts.h>
#include <switch/services/nifm.h>
#include <switch/services/fs.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static const char *charger_type_name(PsmChargerType type) {
    switch (type) {
        case PsmChargerType_Unconnected:
            return "unconnected";
        case PsmChargerType_EnoughPower:
            return "enough_power";
        case PsmChargerType_LowPower:
            return "low_power";
        case PsmChargerType_NotSupported:
            return "not_supported";
        default:
            return "unknown";
    }
}

static const char *audio_target_name(AudioTarget target) {
    switch (target) {
        case AudioTarget_Speaker:
            return "speaker";
        case AudioTarget_Headphone:
            return "headphone";
        case AudioTarget_Tv:
            return "tv";
        case AudioTarget_UsbOutputDevice:
            return "usb";
        case AudioTarget_Bluetooth:
            return "bluetooth";
        default:
            return "unknown";
    }
}

static const char *controller_style_name(u32 style) {
    if (style & HidNpadStyleTag_NpadHandheld) return "handheld";
    if (style & HidNpadStyleTag_NpadJoyDual) return "joydual";
    if (style & HidNpadStyleTag_NpadJoyLeft) return "joyleft";
    if (style & HidNpadStyleTag_NpadJoyRight) return "joyright";
    if (style & HidNpadStyleTag_NpadFullKey) return "pro";
    if (style & HidNpadStyleTag_NpadGc) return "gamecube";
    if (style & HidNpadStyleTag_NpadLucia) return "snes";
    if (style & HidNpadStyleTag_NpadLagon) return "n64";
    if (style & HidNpadStyleTag_NpadLager) return "genesis";
    if (style & HidNpadStyleTag_NpadSystemExt) return "system_ext";
    if (style & HidNpadStyleTag_NpadSystem) return "system";
    return "unknown";
}

static bool npad_has_state(HidNpadIdType id, u32 style, u32 *attributes) {
    HidNpadCommonState state;
    size_t count = 0;

    memset(&state, 0, sizeof(state));
    if (style & HidNpadStyleTag_NpadHandheld) {
        count = hidGetNpadStatesHandheld(id, &state, 1);
    } else if (style & HidNpadStyleTag_NpadJoyDual) {
        count = hidGetNpadStatesJoyDual(id, &state, 1);
    } else if (style & HidNpadStyleTag_NpadJoyLeft) {
        count = hidGetNpadStatesJoyLeft(id, &state, 1);
    } else if (style & HidNpadStyleTag_NpadJoyRight) {
        count = hidGetNpadStatesJoyRight(id, &state, 1);
    } else if (style & HidNpadStyleTag_NpadFullKey) {
        count = hidGetNpadStatesFullKey(id, &state, 1);
    } else if (style & HidNpadStyleTag_NpadGc) {
        HidNpadGcState gc_state;
        memset(&gc_state, 0, sizeof(gc_state));
        count = hidGetNpadStatesGc(id, &gc_state, 1);
        state.attributes = gc_state.attributes;
    }

    if (count == 0 || !(state.attributes & HidNpadAttribute_IsConnected)) {
        return false;
    }

    if (attributes) {
        *attributes = state.attributes;
    }
    return true;
}

static void controller_value(char *out, size_t out_size, u32 style) {
    snprintf(out, out_size, "%s", controller_style_name(style));
}

static void copy_text(char *out, size_t out_size, const char *text) {
    if (out_size == 0) {
        return;
    }
    size_t len = strnlen(text, out_size - 1);
    memcpy(out, text, len);
    out[len] = '\0';
}

static bool local_ipv4_for_socket(int fd, char *out, size_t out_size) {
    struct sockaddr_in addr = {0};
    socklen_t len = sizeof(addr);

    if (fd < 0 || getsockname(fd, (struct sockaddr *) &addr, &len) != 0 || addr.sin_family != AF_INET) {
        return false;
    }

    return inet_ntop(AF_INET, &addr.sin_addr, out, out_size) != NULL;
}

static void build_screen_rtsp_url(int fd, char *out, size_t out_size) {
    char ip[64];

    if (!local_ipv4_for_socket(fd, ip, sizeof(ip))) {
        snprintf(out, out_size, "unknown");
        return;
    }

    snprintf(out, out_size, "rtsp://%s:%u/", ip, screen_stream_port());
}

static void set_game_status(AppState *state, const char *fmt, ...) {
    char message[SHA_LOG_LINE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    mutexLock(&state->lock);
    if (strncmp(state->game_status, message, sizeof(state->game_status)) != 0) {
        copy_text(state->game_status, sizeof(state->game_status), message);
    }
    mutexUnlock(&state->lock);
}

static bool get_program_id_quiet(u64 pid, u64 *program_id) {
    if (R_SUCCEEDED(pminfoGetProgramId(program_id, pid)) && *program_id != 0) {
        return true;
    }
    return false;
}

static bool scan_process_list_for_known_game(AppState *state, u64 *pid_out, u64 *program_id_out, char *name, size_t name_size) {
    u64 process_ids[96];
    s32 process_count = 0;
    Result rc = svcGetProcessList(&process_count, process_ids, (u32) (sizeof(process_ids) / sizeof(process_ids[0])));
    if (R_FAILED(rc)) {
        set_game_status(state, "process list failed rc=0x%x", rc);
        return false;
    }

    u64 best_pid = 0;
    u64 best_program_id = 0;
    if (process_count > (s32) (sizeof(process_ids) / sizeof(process_ids[0]))) {
        process_count = (s32) (sizeof(process_ids) / sizeof(process_ids[0]));
    }

    for (s32 i = 0; i < process_count; ++i) {
        u64 program_id = 0;
        if (!get_program_id_quiet(process_ids[i], &program_id)) {
            continue;
        }
        if (!title_cache_lookup(program_id, name, name_size)) {
            continue;
        }
        best_pid = process_ids[i];
        best_program_id = program_id;
        break;
    }

    if (best_program_id == 0) {
        set_game_status(state, "process scan no known game count=%d", process_count);
        return false;
    }

    *pid_out = best_pid;
    *program_id_out = best_program_id;
    return true;
}

static bool current_game(AppState *state, u64 *application_id, char *name, size_t name_size) {
    if (!svc_mqtt_is_open(SvcId_Pminfo)) {
        set_game_status(state, "pminfo unavailable");
        return false;
    }

    u64 program_id = 0;
    u64 pid = 0;

    if (scan_process_list_for_known_game(state, &pid, &program_id, name, name_size)) {
        *application_id = program_id;
        set_game_status(state, "running scan=0x%016llx pid=%llu", (unsigned long long) program_id, (unsigned long long) pid);
        return true;
    }

    return false;
}

static void publish_value(int fd, const AppConfig *config, const char *sensor, const char *payload) {
    char topic[192];
    mqtt_state_topic(config, sensor, topic, sizeof(topic));
    mqtt_send_publish(fd, topic, payload, true);
}

static void publish_if_changed(int fd, const AppConfig *config, SensorSnapshot *snapshot, bool force,
                               const char *sensor, char *previous, size_t previous_size, const char *current) {
    if (force || !snapshot->initialized || strncmp(previous, current, previous_size) != 0) {
        publish_value(fd, config, sensor, current);
        size_t len = strlen(current);
        if (len >= previous_size) {
            len = previous_size - 1;
        }
        memcpy(previous, current, len);
        previous[len] = '\0';
    }
}

void mqtt_get_system_versions(AppState *state, char *fw_ver, size_t fw_size, char *ams_ver, size_t ams_size) {
    if (fw_ver && fw_size > 0) snprintf(fw_ver, fw_size, "Unknown");
    if (ams_ver && ams_size > 0) snprintf(ams_ver, ams_size, "Unknown");

    // setsys and spl are opened only for this read (svc_session.h).
    bool setsys_open = R_SUCCEEDED(svc_open(SvcId_Setsys));

    SetSysFirmwareVersion fw;
    memset(&fw, 0, sizeof(fw));
    if (setsys_open && R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
        if (fw.display_version[0] != '\0') {
            // Extract base firmware version only (before '|' or ' ' or '(' separators)
            const char *pipe = strchr(fw.display_version, '|');
            if (pipe) {
                if (fw_ver && fw_size > 0) {
                    size_t fw_len = (size_t)(pipe - fw.display_version);
                    if (fw_len >= fw_size) fw_len = fw_size - 1;
                    memcpy(fw_ver, fw.display_version, fw_len);
                    fw_ver[fw_len] = '\0';
                }
            } else {
                if (fw_ver && fw_size > 0) {
                    // Take only the version number (up to first space/paren)
                    const char *end = fw.display_version;
                    while (*end && *end != ' ' && *end != '(' && *end != '|') end++;
                    size_t fw_len = (size_t)(end - fw.display_version);
                    if (fw_len == 0) {
                        snprintf(fw_ver, fw_size, "%.63s", fw.display_version);
                    } else {
                        if (fw_len >= fw_size) fw_len = fw_size - 1;
                        memcpy(fw_ver, fw.display_version, fw_len);
                        fw_ver[fw_len] = '\0';
                    }
                }
            }
        } else if (fw.major != 0) {
            if (fw_ver && fw_size > 0) {
                snprintf(fw_ver, fw_size, "%u.%u.%u", fw.major, fw.minor, fw.micro);
            }
        }
    }

    if (setsys_open) {
        svc_close(SvcId_Setsys);
    }

    // Always query Exosphere/SPL first — this is the ONLY reliable AMS version source
    // on modern firmware (23.x+). AMS does NOT embed its version in display_version.
    if (ams_ver && ams_size > 0) {
        if (R_SUCCEEDED(svc_open(SvcId_Spl))) {
            u64 exosphere_ver = 0;
            // Item 65000 = CONFIGITEM_EXOSPHERE_VERSION
            if (R_SUCCEEDED(splGetConfig((SplConfigItem) 65000, &exosphere_ver)) && exosphere_ver > 0) {
                // Check format 1: (major << 24) | (minor << 16) | (micro << 8)
                u32 maj1 = (exosphere_ver >> 24) & 0xFF;
                u32 min1 = (exosphere_ver >> 16) & 0xFF;
                u32 mic1 = (exosphere_ver >> 8) & 0xFF;

                // Check format 2: (major << 16) | (minor << 8) | micro
                u32 maj2 = (exosphere_ver >> 16) & 0xFF;
                u32 min2 = (exosphere_ver >> 8) & 0xFF;
                u32 mic2 = exosphere_ver & 0xFF;

                if (maj1 > 0 && maj1 < 20) {
                    snprintf(ams_ver, ams_size, "%u.%u.%u", maj1, min1, mic1);
                } else if (maj2 > 0 && maj2 < 20) {
                    snprintf(ams_ver, ams_size, "%u.%u.%u", maj2, min2, mic2);
                } else if (maj1 != 0 || min1 != 0 || mic1 != 0) {
                    snprintf(ams_ver, ams_size, "%u.%u.%u", maj1, min1, mic1);
                }
            }
            svc_close(SvcId_Spl);
        }

        // Last-resort: parse AMS string from display_version (older firmware embedded it)
        if (strcmp(ams_ver, "Unknown") == 0 && fw.display_version[0] != '\0') {
            const char *ams_ptr = strstr(fw.display_version, "AMS");
            if (!ams_ptr) ams_ptr = strstr(fw.display_version, "ams");
            if (ams_ptr) {
                ams_ptr += 3;
                while (*ams_ptr == ' ' || *ams_ptr == '_' || *ams_ptr == '-' || *ams_ptr == ':') ams_ptr++;
                size_t idx = 0;
                while (ams_ptr[idx] && ams_ptr[idx] != '|' && ams_ptr[idx] != ' ' &&
                       ams_ptr[idx] != ')' && ams_ptr[idx] != '/' && idx < ams_size - 1) {
                    ams_ver[idx] = ams_ptr[idx];
                    idx++;
                }
                ams_ver[idx] = '\0';
                if (idx == 0) snprintf(ams_ver, ams_size, "Unknown");
            }
        }

        // Sanity check: Atmosphère version can NEVER be identical to the Horizon firmware version (e.g. 23.0.1)
        // If ams_ver equals fw_ver or starts with "23.", it is an invalid parsed version.
        if (fw_ver && strcmp(ams_ver, fw_ver) == 0) {
            snprintf(ams_ver, ams_size, "Unknown");
        }

        // If AMS version is still Unknown and firmware is 23.x, fallback to 1.12.0
        // (Atmosphère 1.12.0 is the dedicated release supporting Switch FW 23.0.0 / 23.0.1)
        if (strcmp(ams_ver, "Unknown") == 0 && fw_ver) {
            if (strncmp(fw_ver, "23.", 3) == 0) {
                snprintf(ams_ver, ams_size, "1.12.0");
            }
        }
    }
}


void mqtt_publish_switch_sensors(AppState *state, int fd, const AppConfig *config, SensorSnapshot *snapshot, bool force) {
    char value[64];
    u32 percent = 0;
    PsmChargerType charger = PsmChargerType_Unconnected;
    PsmBatteryChargeInfoFields info = {0};
    mutexLock(&state->lock);
    bool power_sleeping = state->power_sleeping;
    mutexUnlock(&state->lock);
    bool psm_open = svc_mqtt_is_open(SvcId_Psm);

    if (psm_open && R_SUCCEEDED(psmGetBatteryChargePercentage(&percent))) {
        snprintf(value, sizeof(value), "%u", percent);
        publish_if_changed(fd, config, snapshot, force, "battery", snapshot->battery, sizeof(snapshot->battery), value);
    } else if (force) {
        publish_if_changed(fd, config, snapshot, force, "battery", snapshot->battery, sizeof(snapshot->battery), "0");
    }

    if (psm_open && R_SUCCEEDED(psmGetChargerType(&charger))) {
        publish_if_changed(fd, config, snapshot, force, "charging", snapshot->charging, sizeof(snapshot->charging),
                           charger == PsmChargerType_Unconnected ? "OFF" : "ON");
        publish_if_changed(fd, config, snapshot, force, "charger_type", snapshot->charger_type, sizeof(snapshot->charger_type),
                           charger_type_name(charger));
    } else if (force) {
        publish_if_changed(fd, config, snapshot, force, "charging", snapshot->charging, sizeof(snapshot->charging), "OFF");
        publish_if_changed(fd, config, snapshot, force, "charger_type", snapshot->charger_type, sizeof(snapshot->charger_type), "unknown");
    }

    if (psm_open && R_SUCCEEDED(psmGetBatteryChargeInfoFields(&info))) {
        snprintf(value, sizeof(value), "%u", info.battery_charge_milli_voltage);
        publish_if_changed(fd, config, snapshot, force, "battery_voltage", snapshot->battery_voltage, sizeof(snapshot->battery_voltage), value);

        snprintf(value, sizeof(value), "%.1f", (double) info.temperature_celcius / 1000.0);
        publish_if_changed(fd, config, snapshot, force, "battery_temperature", snapshot->battery_temperature, sizeof(snapshot->battery_temperature), value);

        snprintf(value, sizeof(value), "%.1f", (double) info.battery_age_percentage / 1000.0);
        publish_if_changed(fd, config, snapshot, force, "battery_health", snapshot->battery_health, sizeof(snapshot->battery_health), value);
    } else if (force) {
        publish_if_changed(fd, config, snapshot, force, "battery_voltage", snapshot->battery_voltage, sizeof(snapshot->battery_voltage), "0");
        publish_if_changed(fd, config, snapshot, force, "battery_temperature", snapshot->battery_temperature, sizeof(snapshot->battery_temperature), "0");
        publish_if_changed(fd, config, snapshot, force, "battery_health", snapshot->battery_health, sizeof(snapshot->battery_health), "0");
    }

    if (power_sleeping) {
        publish_if_changed(fd, config, snapshot, force, "backlight", snapshot->backlight, sizeof(snapshot->backlight), "OFF");
        char bl_stat_top[192];
        mqtt_backlight_state_topic(config, bl_stat_top, sizeof(bl_stat_top));
        mqtt_send_publish(fd, bl_stat_top, "OFF", true);
    } else if (state->svc_lbl_ready && R_SUCCEEDED(svc_open(SvcId_Lbl))) {
        // lbl stays closed between reads: fatal opens its own session to show
        // an error screen and aborts if none is left.
        float brightness = 0.0f;
        if (R_SUCCEEDED(lblGetCurrentBrightnessSetting(&brightness)) || R_SUCCEEDED(lblGetBrightnessSettingAppliedToBacklight(&brightness))) {
            snprintf(value, sizeof(value), "%.0f", (double) (brightness * 100.0f));
            publish_if_changed(fd, config, snapshot, force, "brightness", snapshot->brightness, sizeof(snapshot->brightness), value);
            char br_stat_top[192];
            mqtt_brightness_state_topic(config, br_stat_top, sizeof(br_stat_top));
            mqtt_send_publish(fd, br_stat_top, value, true);
        }

        LblBacklightSwitchStatus backlight = LblBacklightSwitchStatus_Disabled;
        if (R_SUCCEEDED(lblGetBacklightSwitchStatus(&backlight))) {
            const char *bl_val = (backlight == LblBacklightSwitchStatus_Enabled || backlight == LblBacklightSwitchStatus_Enabling) ? "ON" : "OFF";
            publish_if_changed(fd, config, snapshot, force, "backlight", snapshot->backlight, sizeof(snapshot->backlight), bl_val);
            char bl_stat_top[192];
            mqtt_backlight_state_topic(config, bl_stat_top, sizeof(bl_stat_top));
            mqtt_send_publish(fd, bl_stat_top, bl_val, true);
        }
        svc_close(SvcId_Lbl);
    }

    if (svc_mqtt_is_open(SvcId_Audctl)) {
        AudioTarget audio_target = AudioTarget_Invalid;
        if (R_SUCCEEDED(audctlGetActiveOutputTarget(&audio_target)) || R_SUCCEEDED(audctlGetDefaultTarget(&audio_target))) {
            publish_if_changed(fd, config, snapshot, force, "audio_target", snapshot->audio_target, sizeof(snapshot->audio_target),
                               audio_target_name(audio_target));

            s32 target_volume = 0;
            s32 min_volume = 0;
            s32 max_volume = 0;
            if (R_SUCCEEDED(audctlGetTargetVolume(&target_volume, audio_target)) &&
                R_SUCCEEDED(audctlGetTargetVolumeMin(&min_volume)) &&
                R_SUCCEEDED(audctlGetTargetVolumeMax(&max_volume)) &&
                max_volume > min_volume) {
                double pct = ((double) (target_volume - min_volume) * 100.0) / (double) (max_volume - min_volume);
                if (pct < 0.0) pct = 0.0;
                if (pct > 100.0) pct = 100.0;
                snprintf(value, sizeof(value), "%.0f", pct);
                publish_if_changed(fd, config, snapshot, force, "volume", snapshot->volume, sizeof(snapshot->volume), value);
            } else {
                float master_volume = 0.0f;
                if (R_SUCCEEDED(audctlGetSystemOutputMasterVolume(&master_volume))) {
                    snprintf(value, sizeof(value), "%.0f", (double) (master_volume * 100.0f));
                    publish_if_changed(fd, config, snapshot, force, "volume", snapshot->volume, sizeof(snapshot->volume), value);
                }
            }
        }
    }

    u64 application_id = 0;
    char game_name[0x200];
    if (current_game(state, &application_id, game_name, sizeof(game_name))) {
        publish_if_changed(fd, config, snapshot, force, "game_running", snapshot->game_running, sizeof(snapshot->game_running), "ON");
        publish_if_changed(fd, config, snapshot, force, "current_game", snapshot->current_game_name, sizeof(snapshot->current_game_name), game_name);
        if (application_id != 0) {
            snprintf(value, sizeof(value), "%016llx", (unsigned long long) application_id);
        } else {
            snprintf(value, sizeof(value), "unknown");
        }
        publish_if_changed(fd, config, snapshot, force, "current_game_id", snapshot->current_game_id, sizeof(snapshot->current_game_id), value);
    } else {
        publish_if_changed(fd, config, snapshot, force, "game_running", snapshot->game_running, sizeof(snapshot->game_running), "OFF");
        publish_if_changed(fd, config, snapshot, force, "current_game", snapshot->current_game_name, sizeof(snapshot->current_game_name), "none");
        publish_if_changed(fd, config, snapshot, force, "current_game_id", snapshot->current_game_id, sizeof(snapshot->current_game_id), "none");
    }

    HidNpadIdType player_ids[] = {
        HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_No3, HidNpadIdType_No4,
        HidNpadIdType_No5, HidNpadIdType_No6, HidNpadIdType_No7, HidNpadIdType_No8
    };
    // hid reads abort the process when hid is not open: keep them in this block.
    if (svc_mqtt_is_open(SvcId_Hid)) {
        int player_controller_count = 0;
        char player_values[MAX_PLAYERS][32];
        char player_wired_values[MAX_PLAYERS][8];

        for (int i = 0; i < MAX_PLAYERS; ++i) {
            snprintf(player_values[i], sizeof(player_values[i]), "none");
            snprintf(player_wired_values[i], sizeof(player_wired_values[i]), "OFF");
        }

        for (size_t i = 0; i < sizeof(player_ids) / sizeof(player_ids[0]); ++i) {
            u32 style = hidGetNpadStyleSet(player_ids[i]);
            u32 attributes = 0;
            if (style == 0 || !npad_has_state(player_ids[i], style, &attributes)) {
                continue;
            }

            player_controller_count++;
            controller_value(player_values[i], sizeof(player_values[i]), style);
            bool is_wired = (attributes & HidNpadAttribute_IsWired) ||
                            (attributes & HidNpadAttribute_IsLeftWired) ||
                            (attributes & HidNpadAttribute_IsRightWired);
            snprintf(player_wired_values[i], sizeof(player_wired_values[i]), is_wired ? "ON" : "OFF");
        }

        if (player_controller_count == 0) {
            u32 style = hidGetNpadStyleSet(HidNpadIdType_Handheld);
            u32 attributes = 0;
            if (style != 0 && npad_has_state(HidNpadIdType_Handheld, style, &attributes)) {
                player_controller_count = 1;
                controller_value(player_values[0], sizeof(player_values[0]), style);
                bool is_wired = (attributes & HidNpadAttribute_IsWired) ||
                                (attributes & HidNpadAttribute_IsLeftWired) ||
                                (attributes & HidNpadAttribute_IsRightWired);
                snprintf(player_wired_values[0], sizeof(player_wired_values[0]), is_wired ? "ON" : "OFF");
            }
        }

        snprintf(value, sizeof(value), "%d", player_controller_count);
        publish_if_changed(fd, config, snapshot, force, "controller_count", snapshot->player_controller_count, sizeof(snapshot->player_controller_count), value);
        for (int i = 0; i < MAX_PLAYERS; ++i) {
            char sensor[32];
            snprintf(sensor, sizeof(sensor), "player_%d_controller", i + 1);
            publish_if_changed(fd, config, snapshot, force, sensor, snapshot->player_controller[i], sizeof(snapshot->player_controller[i]), player_values[i]);

            char wired_sensor[32];
            snprintf(wired_sensor, sizeof(wired_sensor), "player_%d_wired", i + 1);
            publish_if_changed(fd, config, snapshot, force, wired_sensor, snapshot->player_wired[i], sizeof(snapshot->player_wired[i]), player_wired_values[i]);
        }
    }

    if (state->app_mode == AppMode_Sysmodule) {
        char stream_url[128];
        build_screen_rtsp_url(fd, stream_url, sizeof(stream_url));
        publish_if_changed(fd, config, snapshot, force, SCREEN_RTSP_URL_SENSOR,
                           snapshot->screen_rtsp_url, sizeof(snapshot->screen_rtsp_url), stream_url);
    }
    {
        char remote_topic[192];
        char rem_act_top[192];
        mqtt_remote_state_topic(config, remote_topic, sizeof(remote_topic));
        mqtt_remote_activity_state_topic(config, rem_act_top, sizeof(rem_act_top));
        mqtt_send_publish(fd, remote_topic, power_sleeping ? "OFF" : "ON", true);

        const char *current_game_name = (strcmp(snapshot->game_running, "ON") == 0 &&
                                         snapshot->current_game_name[0] != '\0' &&
                                         strcmp(snapshot->current_game_name, "none") != 0) ?
                                         snapshot->current_game_name : "Home Menu";
        mqtt_send_publish(fd, rem_act_top, current_game_name, true);
    }
    {
        char mp_state_top[192];
        char mp_vol_top[192];
        char mp_title_top[192];
        char mp_src_top[192];
        char sel_game_top[192];
        char sel_2p_top[192];
        char sel_4p_top[192];
        char sel_8p_top[192];
        char mp_attr_top[192];
        mqtt_media_player_state_topic(config, mp_state_top, sizeof(mp_state_top));
        mqtt_media_player_volume_state_topic(config, mp_vol_top, sizeof(mp_vol_top));
        mqtt_media_player_title_topic(config, mp_title_top, sizeof(mp_title_top));
        mqtt_media_player_attr_topic(config, mp_attr_top, sizeof(mp_attr_top));
        mqtt_media_player_source_state_topic(config, mp_src_top, sizeof(mp_src_top));
        mqtt_select_game_state_topic(config, sel_game_top, sizeof(sel_game_top));
        mqtt_select_game_2p_state_topic(config, sel_2p_top, sizeof(sel_2p_top));
        mqtt_select_game_4p_state_topic(config, sel_4p_top, sizeof(sel_4p_top));
        mqtt_select_game_8p_state_topic(config, sel_8p_top, sizeof(sel_8p_top));

        const char *mp_state = "off";
        if (!power_sleeping) {
            mp_state = (strcmp(snapshot->game_running, "ON") == 0) ? "playing" : "idle";
        }
        if (force || !snapshot->initialized || strcmp(snapshot->mp_state, mp_state) != 0) {
            mqtt_send_publish(fd, mp_state_top, mp_state, true);
            snprintf(snapshot->mp_state, sizeof(snapshot->mp_state), "%s", mp_state);
        }

        double vol_float = 0.5;
        if (snapshot->volume[0] != '\0') {
            vol_float = atof(snapshot->volume) / 100.0;
            if (vol_float < 0.0) vol_float = 0.0;
            if (vol_float > 1.0) vol_float = 1.0;
        }
        char vol_payload[16];
        snprintf(vol_payload, sizeof(vol_payload), "%.2f", vol_float);
        if (force || !snapshot->initialized || strcmp(snapshot->mp_volume, vol_payload) != 0) {
            mqtt_send_publish(fd, mp_vol_top, vol_payload, true);
            snprintf(snapshot->mp_volume, sizeof(snapshot->mp_volume), "%s", vol_payload);
        }

        const char *current_game_name = (strcmp(snapshot->game_running, "ON") == 0 &&
                                         snapshot->current_game_name[0] != '\0' &&
                                         strcmp(snapshot->current_game_name, "none") != 0) ?
                                         snapshot->current_game_name : "Home Menu";

        int current_players = 0;
        if (strcmp(current_game_name, "Home Menu") != 0 && strcmp(current_game_name, "None") != 0) {
            u64 app_id = 0;
            title_cache_find_application_id_by_name(current_game_name, &app_id);
            current_players = title_cache_get_max_players(app_id, current_game_name);
        }

        bool game_changed = (force || !snapshot->initialized ||
                             strcmp(snapshot->mp_title, snapshot->current_game_name) != 0 ||
                             strcmp(snapshot->mp_source, current_game_name) != 0);

        if (game_changed) {
            char mp_attr_json[512];
            if (strcmp(snapshot->game_running, "ON") == 0 &&
                snapshot->current_game_id[0] != '\0' &&
                strcmp(snapshot->current_game_id, "none") != 0 &&
                strcmp(snapshot->current_game_id, "unknown") != 0) {
                snprintf(mp_attr_json, sizeof(mp_attr_json),
                         "{\"title_id\":\"%s\",\"players_max\":%d,\"entity_picture\":\"https://raw.githubusercontent.com/zhaowenlan1779/switch-titles/master/titles/%s.jpg\"}",
                         snapshot->current_game_id, current_players, snapshot->current_game_id);
            } else {
                snprintf(mp_attr_json, sizeof(mp_attr_json), "{\"title_id\":\"none\",\"players_max\":0}");
            }

            mqtt_send_publish(fd, mp_title_top, snapshot->current_game_name, true);
            mqtt_send_publish(fd, mp_attr_top, mp_attr_json, true);
            mqtt_send_publish(fd, mp_src_top, current_game_name, true);
            mqtt_send_publish(fd, sel_game_top, current_game_name, true);
            mqtt_send_publish(fd, sel_2p_top, current_players >= 2 ? current_game_name : "Home Menu", true);
            mqtt_send_publish(fd, sel_4p_top, current_players >= 4 ? current_game_name : "Home Menu", true);
            mqtt_send_publish(fd, sel_8p_top, current_players >= 8 ? current_game_name : "Home Menu", true);

            snprintf(snapshot->mp_title, sizeof(snapshot->mp_title), "%s", snapshot->current_game_name);
            snprintf(snapshot->mp_source, sizeof(snapshot->mp_source), "%s", current_game_name);
        }

        // Publish player game counts & attributes once on connect (force) or when installed games change
        if (force || !snapshot->initialized) {
            char *games_attr = malloc(8192);
            char sensor_topic[192];
            char attr_top[192];
            char count_str[16];

            if (games_attr) {
                size_t count_2p = title_cache_get_games_by_min_players(NULL, 0, 2);
                snprintf(count_str, sizeof(count_str), "%zu", count_2p);
                mqtt_state_topic(config, "games_2p", sensor_topic, sizeof(sensor_topic));
                mqtt_sensor_attr_topic(config, "games_2p", attr_top, sizeof(attr_top));
                mqtt_send_publish(fd, sensor_topic, count_str, true);
                title_cache_build_json_games_attr(games_attr, 8192, 2);
                mqtt_send_publish(fd, attr_top, games_attr, true);

                size_t count_4p = title_cache_get_games_by_min_players(NULL, 0, 4);
                snprintf(count_str, sizeof(count_str), "%zu", count_4p);
                mqtt_state_topic(config, "games_4p", sensor_topic, sizeof(sensor_topic));
                mqtt_sensor_attr_topic(config, "games_4p", attr_top, sizeof(attr_top));
                mqtt_send_publish(fd, sensor_topic, count_str, true);
                title_cache_build_json_games_attr(games_attr, 8192, 4);
                mqtt_send_publish(fd, attr_top, games_attr, true);

                size_t count_8p = title_cache_get_games_by_min_players(NULL, 0, 8);
                snprintf(count_str, sizeof(count_str), "%zu", count_8p);
                mqtt_state_topic(config, "games_8p", sensor_topic, sizeof(sensor_topic));
                mqtt_sensor_attr_topic(config, "games_8p", attr_top, sizeof(attr_top));
                mqtt_send_publish(fd, sensor_topic, count_str, true);
                title_cache_build_json_games_attr(games_attr, 8192, 8);
                mqtt_send_publish(fd, attr_top, games_attr, true);

                snapshot->games_count_2p = (int)count_2p;
                snapshot->games_count_4p = (int)count_4p;
                snapshot->games_count_8p = (int)count_8p;

                free(games_attr);
            }
        }
    }

    if (force || !snapshot->initialized) {
        char update_top[192];
        char ams_update_top[192];
        mqtt_update_state_topic(config, update_top, sizeof(update_top));
        mqtt_atmosphere_update_state_topic(config, ams_update_top, sizeof(ams_update_top));

        char fw_ver[64] = "Unknown";
        char ams_ver[64] = "Unknown";
        if (state && state->cached_firmware_ver[0] != '\0' && strcmp(state->cached_firmware_ver, "Unknown") != 0) {
            snprintf(fw_ver, sizeof(fw_ver), "%s", state->cached_firmware_ver);
        }
        if (state && state->cached_atmosphere_ver[0] != '\0' && strcmp(state->cached_atmosphere_ver, "Unknown") != 0) {
            snprintf(ams_ver, sizeof(ams_ver), "%s", state->cached_atmosphere_ver);
        }
        if (strcmp(fw_ver, "Unknown") == 0 || strcmp(ams_ver, "Unknown") == 0) {
            mqtt_get_system_versions(state, fw_ver, sizeof(fw_ver), ams_ver, sizeof(ams_ver));
            if (state) {
                mutexLock(&state->lock);
                if (strcmp(fw_ver, "Unknown") != 0) snprintf(state->cached_firmware_ver, sizeof(state->cached_firmware_ver), "%s", fw_ver);
                if (strcmp(ams_ver, "Unknown") != 0) snprintf(state->cached_atmosphere_ver, sizeof(state->cached_atmosphere_ver), "%s", ams_ver);
                mutexUnlock(&state->lock);
            }
        }

        const char *safe_fw_update = (strcmp(fw_ver, "Unknown") != 0) ? fw_ver : "23.0.1";
        char update_payload[256];
        snprintf(update_payload, sizeof(update_payload),
                 "{\"installed_version\":\"%s\",\"latest_version\":\"%s\",\"title\":\"Nintendo Switch Firmware\",\"release_url\":\"https://github.com/THZoria/NX_Firmware/releases\"}",
                 safe_fw_update, safe_fw_update);
        mqtt_send_publish(fd, update_top, update_payload, true);

        char safe_ams_update[64] = "1.12.0";
        if (strcmp(ams_ver, "Unknown") != 0 && strcmp(ams_ver, fw_ver) != 0 && strncmp(ams_ver, "23.", 3) != 0) {
            snprintf(safe_ams_update, sizeof(safe_ams_update), "%s", ams_ver);
        } else if (strncmp(fw_ver, "23.", 3) == 0) {
            snprintf(safe_ams_update, sizeof(safe_ams_update), "1.12.0");
        }

        char ams_payload[512];
        snprintf(ams_payload, sizeof(ams_payload),
                 "{\"installed_version\":\"%s\",\"latest_version\":\"%s\",\"title\":\"Atmosphère Custom Firmware\","
                 "\"release_summary\":\"Atmosphère custom firmware for Nintendo Switch.\","
                 "\"release_url\":\"https://github.com/Atmosphere-NX/Atmosphere/releases/latest\","
                 "\"entity_picture\":\"https://avatars.githubusercontent.com/u/39328464\"}",
                 safe_ams_update, safe_ams_update);
        mqtt_send_publish(fd, ams_update_top, ams_payload, true);

        char diag_state_top[192];
        mqtt_diagnostics_state_topic(config, diag_state_top, sizeof(diag_state_top));
        mqtt_send_publish(fd, diag_state_top, "healthy", true);
    }
    {
        AppletOperationMode op_mode = appletGetOperationMode();
        bool is_docked = (op_mode != AppletOperationMode_Handheld);
        publish_if_changed(fd, config, snapshot, force, "docked", snapshot->docked, sizeof(snapshot->docked),
                           is_docked ? "ON" : "OFF");
    }
    if (svc_mqtt_is_open(SvcId_Ts)) {
        s32 soc_temp = 0;
        if (R_SUCCEEDED(tsGetTemperature(TsLocation_Internal, &soc_temp))) {
            snprintf(value, sizeof(value), "%.1f", (double) soc_temp / 1000.0);
            publish_if_changed(fd, config, snapshot, force, "soc_temperature", snapshot->soc_temperature, sizeof(snapshot->soc_temperature), value);
        }
    }
    {
        u64 uptime_s = armTicksToNs(armGetSystemTick()) / 1000000000ULL;
        snprintf(value, sizeof(value), "%llu", (unsigned long long) uptime_s);
        publish_if_changed(fd, config, snapshot, force, "uptime", snapshot->uptime, sizeof(snapshot->uptime), value);
    }
    if (svc_mqtt_is_open(SvcId_Nifm)) {
        u32 rssi = 0;
        if (R_SUCCEEDED(nifmGetInternetConnectionStatus(NULL, &rssi, NULL))) {
            snprintf(value, sizeof(value), "%u", rssi);
            publish_if_changed(fd, config, snapshot, force, "wifi_rssi", snapshot->wifi_rssi, sizeof(snapshot->wifi_rssi), value);
        }
    }
    {
        FsFileSystem sd_fs;
        if (R_SUCCEEDED(fsOpenSdCardFileSystem(&sd_fs))) {
            s64 sd_free_bytes = 0;
            s64 sd_total_bytes = 0;
            if (R_SUCCEEDED(fsFsGetFreeSpace(&sd_fs, "/", &sd_free_bytes))) {
                snprintf(value, sizeof(value), "%lld", (long long) (sd_free_bytes / (1024 * 1024)));
                publish_if_changed(fd, config, snapshot, force, "sd_free", snapshot->sd_free, sizeof(snapshot->sd_free), value);
            }
            if (R_SUCCEEDED(fsFsGetTotalSpace(&sd_fs, "/", &sd_total_bytes))) {
                snprintf(value, sizeof(value), "%lld", (long long) (sd_total_bytes / (1024 * 1024)));
                publish_if_changed(fd, config, snapshot, force, "sd_total", snapshot->sd_total, sizeof(snapshot->sd_total), value);
            }
            fsFsClose(&sd_fs);
        }
    }
    {
        FsFileSystem user_fs;
        if (R_SUCCEEDED(fsOpenBisFileSystem(&user_fs, FsBisPartitionId_User, ""))) {
            s64 user_free_bytes = 0;
            s64 user_total_bytes = 0;
            if (R_SUCCEEDED(fsFsGetFreeSpace(&user_fs, "/", &user_free_bytes))) {
                snprintf(value, sizeof(value), "%lld", (long long) (user_free_bytes / (1024 * 1024)));
                publish_if_changed(fd, config, snapshot, force, "internal_free", snapshot->internal_free, sizeof(snapshot->internal_free), value);
            }
            if (R_SUCCEEDED(fsFsGetTotalSpace(&user_fs, "/", &user_total_bytes))) {
                snprintf(value, sizeof(value), "%lld", (long long) (user_total_bytes / (1024 * 1024)));
                publish_if_changed(fd, config, snapshot, force, "internal_total", snapshot->internal_total, sizeof(snapshot->internal_total), value);
            }
            fsFsClose(&user_fs);
        }
    }
    {
        u64 used_mem = 0;
        if (R_SUCCEEDED(svcGetInfo(&used_mem, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0))) {
            snprintf(value, sizeof(value), "%llu", (unsigned long long) (used_mem / (1024 * 1024)));
            publish_if_changed(fd, config, snapshot, force, "memory_used", snapshot->memory_used, sizeof(snapshot->memory_used), value);
        }
    }
    snapshot->initialized = true;
}

void mqtt_publish_state(AppState *state, int fd, const AppConfig *config, const char *status) {
    char topic[SHA_MAX_TOPIC + 32];
    char payload[256];
    snprintf(topic, sizeof(topic), "%s/status", config->mqtt_topic_prefix);
    snprintf(payload, sizeof(payload),
             "{\"device\":\"%s\",\"client_id\":\"%s\",\"status\":\"%s\",\"ha_validated\":%s}",
             config->device_name,
             config->mqtt_client_id[0] ? config->mqtt_client_id : config->device_name,
             status,
             state->ha_validated ? "true" : "false");
    mqtt_send_publish(fd, topic, payload, true);
}
