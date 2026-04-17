#include "ui.h"

#include "config.h"
#include "http_client.h"
#include "mqtt_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C_RESET "\x1b[0m"
#define C_DIM "\x1b[2m"
#define C_RED "\x1b[31m"
#define C_GREEN "\x1b[32m"
#define C_YELLOW "\x1b[33m"
#define C_CYAN "\x1b[36m"
#define C_BOLD "\x1b[1m"

static bool prompt_text(const char *guide, char *buffer, size_t size, bool secret) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) {
        return false;
    }

    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetGuideText(&kbd, guide);
    swkbdConfigSetInitialText(&kbd, buffer);
    swkbdConfigSetStringLenMax(&kbd, size - 1);
    if (secret) {
        swkbdConfigSetPasswordFlag(&kbd, true);
    }

    Result rc = swkbdShow(&kbd, buffer, size);
    swkbdClose(&kbd);
    return R_SUCCEEDED(rc);
}

static bool prompt_number(const char *guide, int *value) {
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d", *value);
    if (!prompt_text(guide, buffer, sizeof(buffer), false)) {
        return false;
    }
    *value = atoi(buffer);
    return true;
}

static void save_config_with_status(AppState *state, const AppConfig *config) {
    if (config_save(config)) {
        mutexLock(&state->lock);
        state->config_dirty = false;
        mutexUnlock(&state->lock);
        app_state_push_log(state, "Config saved");
        app_state_set_status(state, "Saved to %s", config_path());
    } else {
        app_state_push_log(state, "Save failed: %s", config_last_error());
        app_state_set_status(state, "Save failed");
    }
}

static const char *flag(bool ok) {
    return ok ? C_GREEN "[OK]" C_RESET : C_RED "[X]" C_RESET;
}

static const char *cursor(int selected, int row) {
    return selected == row ? C_YELLOW ">" C_RESET : " ";
}

static void test_connections(AppState *state) {
    app_state_push_log(state, "Connection test started");
    app_state_set_status(state, "Testing HA and MQTT...");
    http_validate_home_assistant(state);
    mqtt_test_connection(state);
}

static void draw_ui(AppState *state, int selected) {
    consoleClear();
    printf(C_BOLD C_CYAN "Switch Assistant" C_RESET "  " C_DIM "build %s" C_RESET "\n", SHA_APP_BUILD);
    printf(C_DIM "D-Pad select   A edit   Y test HA+MQTT   - reboot   + exit" C_RESET "\n");
    printf(C_YELLOW "Restart with - to apply changes. You can also edit config.ini from your PC." C_RESET "\n");
    printf("----------------------------------------------------------------\n\n");

    mutexLock(&state->lock);
    printf(C_BOLD "Home Assistant" C_RESET "\n");
    printf("%s URL       : %s\n", cursor(selected, 0), state->config.ha_url);
    printf("%s Token     : %s\n", cursor(selected, 1), state->config.ha_token[0] ? "********" : C_DIM "<empty>" C_RESET);
    printf("\n");

    printf(C_BOLD "MQTT Broker" C_RESET "\n");
    printf("%s Host IP   : %s\n", cursor(selected, 2), state->config.mqtt_host);
    printf("%s Port      : %d\n", cursor(selected, 3), state->config.mqtt_port);
    printf("%s Username  : %s\n", cursor(selected, 4), state->config.mqtt_username[0] ? state->config.mqtt_username : C_DIM "<empty>" C_RESET);
    printf("%s Password  : %s\n", cursor(selected, 5), state->config.mqtt_password[0] ? "********" : C_DIM "<empty>" C_RESET);
    printf("\n");

    printf(C_BOLD "Device" C_RESET "\n");
    printf("%s Discovery : %s\n", cursor(selected, 6), state->config.mqtt_topic_prefix);
    printf("%s Name      : %s\n", cursor(selected, 7), state->config.device_name);
    printf("%s Client ID : %s\n", cursor(selected, 8), state->config.mqtt_client_id[0] ? state->config.mqtt_client_id : state->config.device_name);
    printf("\n");

    printf(C_BOLD "Status" C_RESET "  " C_DIM "config %s" C_RESET "\n", state->config_status);
    printf("  %s HA test   %s MQTT test\n",
           flag(state->ha_validated),
           flag(state->mqtt_connected));
    printf("\n");

    printf(C_BOLD "Details" C_RESET "\n");
    printf("  Status : %s\n", state->status_line);
    printf("  MQTT   : %s\n", state->mqtt_last_error);
    printf("\nRecent log:\n");
    for (int i = 0; i < state->log_count; ++i) {
        printf("- %s\n", state->log_lines[i]);
    }
    mutexUnlock(&state->lock);
    consoleUpdate(NULL);
}

void ui_run(AppState *state) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);

    PadState pad;
    padInitializeDefault(&pad);

    int selected = 0;
    const int field_count = 9;

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);

        if (down & HidNpadButton_Plus) {
            break;
        }
        if (down & HidNpadButton_Minus) {
            app_state_push_log(state, "Reboot requested");
            app_state_set_status(state, "Rebooting console...");
            draw_ui(state, selected);

            Result rc = spsmInitialize();
            if (R_SUCCEEDED(rc)) {
                spsmShutdown(true);
                spsmExit();
            } else {
                app_state_push_log(state, "Reboot failed: 0x%x", rc);
                app_state_set_status(state, "Reboot failed: 0x%x", rc);
            }
        }
        if (down & HidNpadButton_Down) {
            selected = (selected + 1) % field_count;
        }
        if (down & HidNpadButton_Up) {
            selected = (selected + field_count - 1) % field_count;
        }
        if (down & HidNpadButton_Y) {
            test_connections(state);
        }
        if (down & HidNpadButton_A) {
            mutexLock(&state->lock);
            AppConfig tmp = state->config;
            mutexUnlock(&state->lock);

            bool changed = false;
            switch (selected) {
                case 0:
                    changed = prompt_text("Home Assistant URL", tmp.ha_url, sizeof(tmp.ha_url), false);
                    break;
                case 1:
                    changed = prompt_text("Long-lived access token", tmp.ha_token, sizeof(tmp.ha_token), true);
                    break;
                case 2:
                    changed = prompt_text("MQTT host IP only", tmp.mqtt_host, sizeof(tmp.mqtt_host), false);
                    break;
                case 3:
                    changed = prompt_number("MQTT port", &tmp.mqtt_port);
                    break;
                case 4:
                    changed = prompt_text("MQTT username", tmp.mqtt_username, sizeof(tmp.mqtt_username), false);
                    break;
                case 5:
                    changed = prompt_text("MQTT password", tmp.mqtt_password, sizeof(tmp.mqtt_password), true);
                    break;
                case 6:
                    changed = prompt_text("MQTT discovery prefix (usually homeassistant)", tmp.mqtt_topic_prefix, sizeof(tmp.mqtt_topic_prefix), false);
                    break;
                case 7:
                    changed = prompt_text("Device name", tmp.device_name, sizeof(tmp.device_name), false);
                    break;
                case 8:
                    changed = prompt_text("MQTT client ID", tmp.mqtt_client_id, sizeof(tmp.mqtt_client_id), false);
                    break;
            }

            if (changed) {
                mutexLock(&state->lock);
                state->config = tmp;
                state->config_dirty = true;
                state->restart_required = true;
                mutexUnlock(&state->lock);
                app_state_push_log(state, "Field updated");
                save_config_with_status(state, &tmp);
            }
        }

        draw_ui(state, selected);
        svcSleepThread(16 * 1000 * 1000ULL);
    }

    consoleExit(NULL);
}
