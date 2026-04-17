#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define CONFIG_DIR "sdmc:/switch/switch-ha"
#define CONFIG_PATH CONFIG_DIR "/config.ini"

static char g_last_error[128] = "no error";

static void set_error(const char *action) {
    snprintf(g_last_error, sizeof(g_last_error), "%s: errno=%d", action, errno);
}

const char *config_path(void) {
    return CONFIG_PATH;
}

const char *config_last_error(void) {
    return g_last_error;
}

static bool ensure_config_dir(void) {
    errno = 0;
    if (mkdir("sdmc:/switch", 0777) != 0 && errno != EEXIST) {
        set_error("create sdmc:/switch failed");
        return false;
    }

    errno = 0;
    if (mkdir(CONFIG_DIR, 0777) != 0 && errno != EEXIST) {
        set_error("create config dir failed");
        return false;
    }

    return true;
}

static void trim_eol(char *value) {
    size_t len = strlen(value);
    while (len > 0 && (value[len - 1] == '\n' || value[len - 1] == '\r')) {
        value[--len] = '\0';
    }
}

static void config_generate_client_id(AppConfig *config) {
    u64 seed = armGetSystemTick() ^ ((u64) time(NULL) << 32);
    u32 suffix = (u32) (seed ^ (seed >> 32) ^ (seed >> 17));
    snprintf(config->mqtt_client_id, sizeof(config->mqtt_client_id), "switch-ha-%08x", suffix);
}

void config_set_defaults(AppConfig *config) {
    memset(config, 0, sizeof(*config));
    snprintf(config->ha_url, sizeof(config->ha_url), "http://homeassistant.local:8123");
    config->mqtt_host[0] = '\0';
    config->mqtt_port = 1883;
    snprintf(config->mqtt_topic_prefix, sizeof(config->mqtt_topic_prefix), "homeassistant");
    snprintf(config->device_name, sizeof(config->device_name), "Nintendo Switch");
    config_generate_client_id(config);
}

bool config_load(AppConfig *config) {
    config_set_defaults(config);

    FILE *file = fopen(CONFIG_PATH, "r");
    if (!file) {
        set_error("open config for read failed");
        return false;
    }

    char line[512];
    while (fgets(line, sizeof(line), file)) {
        trim_eol(line);
        char *sep = strchr(line, '=');
        if (!sep) {
            continue;
        }
        *sep = '\0';
        const char *key = line;
        const char *value = sep + 1;

        if (strcmp(key, "config_version") == 0 || strcmp(key, "app_build") == 0 || strcmp(key, "mqtt_retain") == 0 || strcmp(key, "mqtt_enabled") == 0) {
            continue;
        } else if (strcmp(key, "ha_url") == 0) {
            strncpy(config->ha_url, value, sizeof(config->ha_url) - 1);
        } else if (strcmp(key, "ha_token") == 0) {
            strncpy(config->ha_token, value, sizeof(config->ha_token) - 1);
        } else if (strcmp(key, "mqtt_host") == 0) {
            strncpy(config->mqtt_host, value, sizeof(config->mqtt_host) - 1);
        } else if (strcmp(key, "mqtt_port") == 0) {
            config->mqtt_port = atoi(value);
        } else if (strcmp(key, "mqtt_username") == 0) {
            strncpy(config->mqtt_username, value, sizeof(config->mqtt_username) - 1);
        } else if (strcmp(key, "mqtt_password") == 0) {
            strncpy(config->mqtt_password, value, sizeof(config->mqtt_password) - 1);
        } else if (strcmp(key, "mqtt_topic_prefix") == 0) {
            strncpy(config->mqtt_topic_prefix, value, sizeof(config->mqtt_topic_prefix) - 1);
        } else if (strcmp(key, "mqtt_client_id") == 0) {
            strncpy(config->mqtt_client_id, value, sizeof(config->mqtt_client_id) - 1);
        } else if (strcmp(key, "device_name") == 0) {
            strncpy(config->device_name, value, sizeof(config->device_name) - 1);
        }
    }

    fclose(file);
    snprintf(g_last_error, sizeof(g_last_error), "no error");
    return true;
}

bool config_save(const AppConfig *config) {
    if (!ensure_config_dir()) {
        return false;
    }

    FILE *file = fopen(CONFIG_PATH, "w");
    if (!file) {
        set_error("open config for write failed");
        return false;
    }

    fprintf(file, "ha_url=%s\n", config->ha_url);
    fprintf(file, "ha_token=%s\n", config->ha_token);
    fprintf(file, "mqtt_host=%s\n", config->mqtt_host);
    fprintf(file, "mqtt_port=%d\n", config->mqtt_port);
    fprintf(file, "mqtt_username=%s\n", config->mqtt_username);
    fprintf(file, "mqtt_password=%s\n", config->mqtt_password);
    fprintf(file, "mqtt_topic_prefix=%s\n", config->mqtt_topic_prefix);
    fprintf(file, "mqtt_client_id=%s\n", config->mqtt_client_id);
    fprintf(file, "device_name=%s\n", config->device_name);

    if (fflush(file) != 0) {
        set_error("flush config failed");
        fclose(file);
        return false;
    }

    if (fclose(file) != 0) {
        set_error("close config failed");
        return false;
    }

    snprintf(g_last_error, sizeof(g_last_error), "no error");
    return true;
}
