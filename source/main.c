#include "app_state.h"
#include "config.h"
#include "http_client.h"
#include "mqtt_client.h"
#include "ui.h"

#include <curl/curl.h>
#include <switch.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define SYSMODULE_TITLE_ID "0100000000000F12"
#define SYSMODULE_SDMC_BASE "sdmc:/atmosphere/contents/" SYSMODULE_TITLE_ID
#define SYSMODULE_SDMC_EXEFS SYSMODULE_SDMC_BASE "/exefs.nsp"
#define SYSMODULE_SDMC_FLAG SYSMODULE_SDMC_BASE "/flags/boot2.flag"

extern const unsigned char switch_ha_sysmodule_exefs_start[];
extern const unsigned char switch_ha_sysmodule_exefs_end[];

static bool ensure_dir(const char *path) {
    if (mkdir(path, 0777) == 0 || errno == EEXIST) {
        return true;
    }
    return false;
}

static bool write_file(const char *dst_path, const unsigned char *data, size_t size) {
    FILE *dst = fopen(dst_path, "wb");
    if (!dst) {
        return false;
    }

    bool ok = fwrite(data, 1, size, dst) == size;
    if (fclose(dst) != 0) {
        ok = false;
    }
    return ok;
}

static bool install_sysmodule(AppState *state) {
    if (!ensure_dir("sdmc:/atmosphere") ||
        !ensure_dir("sdmc:/atmosphere/contents") ||
        !ensure_dir(SYSMODULE_SDMC_BASE) ||
        !ensure_dir(SYSMODULE_SDMC_BASE "/flags")) {
        app_state_push_log(state, "Sysmodule install failed: mkdir");
        return false;
    }

    size_t exefs_size = (size_t) (switch_ha_sysmodule_exefs_end - switch_ha_sysmodule_exefs_start);
    if (exefs_size == 0 || !write_file(SYSMODULE_SDMC_EXEFS, switch_ha_sysmodule_exefs_start, exefs_size)) {
        app_state_push_log(state, "Sysmodule install failed: exefs");
        return false;
    }

    FILE *flag = fopen(SYSMODULE_SDMC_FLAG, "wb");
    if (!flag) {
        app_state_push_log(state, "Sysmodule install failed: boot2");
        return false;
    }
    fclose(flag);

    app_state_push_log(state, "Sysmodule installed");
    return true;
}

int main(int argc, char **argv) {
    fsInitialize();
    fsdevMountSdmc();
    nxlinkStdio();
    socketInitializeDefault();
    curl_global_init(CURL_GLOBAL_DEFAULT);

    AppState state;
    app_state_init(&state);
    install_sysmodule(&state);

    if (config_load(&state.config)) {
        app_state_push_log(&state, "Config loaded");
        snprintf(state.config_status, sizeof(state.config_status), "loaded");
    } else if (config_save(&state.config)) {
        app_state_push_log(&state, "Default config created");
        app_state_set_status(&state, "Config created");
        snprintf(state.config_status, sizeof(state.config_status), "created default");
    } else {
        app_state_push_log(&state, "Config create failed: %s", config_last_error());
        app_state_set_status(&state, "Config create failed");
        snprintf(state.config_status, sizeof(state.config_status), "create failed");
    }

    app_state_push_log(&state, "Initial connection test");
    app_state_set_status(&state, "Testing HA and MQTT...");
    http_validate_home_assistant(&state);
    mqtt_test_connection(&state);

    ui_run(&state);

    curl_global_cleanup();
    socketExit();
    fsdevUnmountDevice("sdmc");
    fsExit();
    return 0;
}
