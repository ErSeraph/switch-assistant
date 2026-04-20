#include "app_state.h"
#include "config.h"
#include "http_client.h"
#include "mqtt_client.h"
#include "title_cache.h"
#include "ui.h"

#include <curl/curl.h>
#include <switch.h>
#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SYSMODULE_TITLE_ID "00FF000053484101"
#define SYSMODULE_SDMC_BASE "sdmc:/atmosphere/contents/" SYSMODULE_TITLE_ID
#define SYSMODULE_SDMC_EXEFS SYSMODULE_SDMC_BASE "/exefs.nsp"
#define SYSMODULE_SDMC_FLAG SYSMODULE_SDMC_BASE "/flags/boot2.flag"
#define OVERLAY_LOADER_TITLE_ID "00FF000053484102"
#define OVERLAY_LOADER_SDMC_BASE "sdmc:/atmosphere/contents/" OVERLAY_LOADER_TITLE_ID
#define OVERLAY_LOADER_SDMC_EXEFS OVERLAY_LOADER_SDMC_BASE "/exefs.nsp"
#define OVERLAY_LOADER_SDMC_FLAG OVERLAY_LOADER_SDMC_BASE "/flags/boot2.flag"
#define OVERLAY_SDMC_PATH "sdmc:/switch/switch-ha/switch-ha-overlay.ovl"
#define LEGACY_SYSMODULE_SDMC_BASE "sdmc:/atmosphere/contents/0100000000000F12"
#define LEGACY_OVERLAY_LOADER_SDMC_BASE "sdmc:/atmosphere/contents/0100000000000F13"

extern const unsigned char switch_ha_sysmodule_exefs_start[];
extern const unsigned char switch_ha_sysmodule_exefs_end[];
extern const unsigned char switch_ha_overlay_loader_exefs_start[];
extern const unsigned char switch_ha_overlay_loader_exefs_end[];
extern const unsigned char switch_ha_overlay_ovl_start[];
extern const unsigned char switch_ha_overlay_ovl_end[];
extern const unsigned char switch_ha_titles_txt_start[];
extern const unsigned char switch_ha_titles_txt_end[];

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

static bool buffer_contains_marker(const unsigned char *buffer, size_t size, const char *marker) {
    size_t marker_len = strlen(marker);
    if (marker_len == 0 || size < marker_len) {
        return false;
    }

    for (size_t i = 0; i <= size - marker_len; ++i) {
        if (memcmp(buffer + i, marker, marker_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool file_contains_marker(const char *path, const char *marker) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return false;
    }

    unsigned char buffer[1024];
    char overlap[64] = {0};
    size_t overlap_len = 0;
    bool found = false;

    while (!found) {
        size_t read = fread(buffer + overlap_len, 1, sizeof(buffer) - overlap_len, file);
        size_t total = overlap_len + read;
        if (total > 0 && buffer_contains_marker(buffer, total, marker)) {
            found = true;
            break;
        }
        if (read == 0) {
            break;
        }

        size_t marker_len = strlen(marker);
        overlap_len = marker_len > 1 && total >= marker_len - 1 ? marker_len - 1 : total;
        memcpy(overlap, buffer + total - overlap_len, overlap_len);
        memcpy(buffer, overlap, overlap_len);
    }

    fclose(file);
    return found;
}

static bool remove_tree(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) {
        return remove(path) == 0 || errno == ENOENT;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child[512];
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);

        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (!remove_tree(child)) {
                closedir(dir);
                return false;
            }
        } else if (remove(child) != 0 && errno != ENOENT) {
            closedir(dir);
            return false;
        }
    }

    closedir(dir);
    return rmdir(path) == 0 || errno == ENOENT;
}

static void remove_legacy_install_if_owned(AppState *state, const char *base_path, const char *marker, const char *label) {
    char exefs_path[256];
    snprintf(exefs_path, sizeof(exefs_path), "%s/exefs.nsp", base_path);

    if (!file_contains_marker(exefs_path, marker)) {
        return;
    }

    if (remove_tree(base_path)) {
        app_state_push_log(state, "Removed old %s install", label);
    } else {
        app_state_push_log(state, "Old %s cleanup failed", label);
    }
}

static bool install_sysmodule(AppState *state) {
    remove_legacy_install_if_owned(state, LEGACY_SYSMODULE_SDMC_BASE, "switch-ha-sysmodule", "sysmodule");
    remove_legacy_install_if_owned(state, LEGACY_OVERLAY_LOADER_SDMC_BASE, "switch-ha-overlay-loader", "overlay loader");

    if (!ensure_dir("sdmc:/switch") ||
        !ensure_dir("sdmc:/switch/switch-ha") ||
        !ensure_dir("sdmc:/atmosphere") ||
        !ensure_dir("sdmc:/atmosphere/contents") ||
        !ensure_dir(SYSMODULE_SDMC_BASE) ||
        !ensure_dir(SYSMODULE_SDMC_BASE "/flags") ||
        !ensure_dir(OVERLAY_LOADER_SDMC_BASE) ||
        !ensure_dir(OVERLAY_LOADER_SDMC_BASE "/flags")) {
        app_state_push_log(state, "Sysmodule install failed: mkdir");
        return false;
    }

    size_t exefs_size = (size_t) (switch_ha_sysmodule_exefs_end - switch_ha_sysmodule_exefs_start);
    if (exefs_size == 0 || !write_file(SYSMODULE_SDMC_EXEFS, switch_ha_sysmodule_exefs_start, exefs_size)) {
        app_state_push_log(state, "Sysmodule install failed: exefs");
        return false;
    }

    size_t overlay_loader_size = (size_t) (switch_ha_overlay_loader_exefs_end - switch_ha_overlay_loader_exefs_start);
    if (overlay_loader_size == 0 || !write_file(OVERLAY_LOADER_SDMC_EXEFS, switch_ha_overlay_loader_exefs_start, overlay_loader_size)) {
        app_state_push_log(state, "Overlay loader install failed");
        return false;
    }

    size_t overlay_size = (size_t) (switch_ha_overlay_ovl_end - switch_ha_overlay_ovl_start);
    if (overlay_size == 0 || !write_file(OVERLAY_SDMC_PATH, switch_ha_overlay_ovl_start, overlay_size)) {
        app_state_push_log(state, "Overlay install failed");
        return false;
    }

    size_t titles_size = (size_t) (switch_ha_titles_txt_end - switch_ha_titles_txt_start);
    if (titles_size == 0 || !write_file(TITLE_CACHE_PATH, switch_ha_titles_txt_start, titles_size)) {
        app_state_push_log(state, "Title database install failed");
        return false;
    }

    FILE *flag = fopen(SYSMODULE_SDMC_FLAG, "wb");
    if (!flag) {
        app_state_push_log(state, "Sysmodule install failed: boot2");
        return false;
    }
    fclose(flag);

    flag = fopen(OVERLAY_LOADER_SDMC_FLAG, "wb");
    if (!flag) {
        app_state_push_log(state, "Overlay loader install failed: boot2");
        return false;
    }
    fclose(flag);

    app_state_push_log(state, "Sysmodules and overlay installed");
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
