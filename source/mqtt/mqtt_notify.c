#include "mqtt/mqtt_notify.h"
#include "mqtt/mqtt_proto.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NOTIFICATION_CURRENT_PATH "sdmc:/switch/switch-ha/notification-current.ini"
#define NOTIFICATION_LOG_PATH "sdmc:/switch/switch-ha/notifications.log"
#define NOTIFICATION_LOG_MAX_BYTES 8192

void mqtt_trim_text(char *value) {
    if (!value) return;
    while (value[0] == ' ' || value[0] == '\t' || value[0] == '\r' || value[0] == '\n') {
        memmove(value, value + 1, strlen(value));
    }

    size_t len = strlen(value);
    while (len > 0) {
        char ch = value[len - 1];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            break;
        }
        value[--len] = '\0';
    }
}

void mqtt_copy_text(char *out, size_t out_size, const char *text) {
    if (out_size == 0) {
        return;
    }
    size_t len = strnlen(text, out_size - 1);
    memcpy(out, text, len);
    out[len] = '\0';
}

static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static bool read_json_hex4(const char *pos, uint32_t *out) {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        int digit = hex_value(pos[i]);
        if (digit < 0) {
            return false;
        }
        value = (value << 4) | (uint32_t) digit;
    }
    *out = value;
    return true;
}

static void append_utf8(char *out, size_t out_size, size_t *used, uint32_t cp) {
    if (*used + 1 >= out_size) {
        return;
    }

    if (cp <= 0x7F) {
        out[(*used)++] = (char) cp;
    } else if (cp <= 0x7FF && *used + 2 < out_size) {
        out[(*used)++] = (char) (0xC0 | ((cp >> 6) & 0x1F));
        out[(*used)++] = (char) (0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF && *used + 3 < out_size) {
        out[(*used)++] = (char) (0xE0 | ((cp >> 12) & 0x0F));
        out[(*used)++] = (char) (0x80 | ((cp >> 6) & 0x3F));
        out[(*used)++] = (char) (0x80 | (cp & 0x3F));
    } else if (cp <= 0x10FFFF && *used + 4 < out_size) {
        out[(*used)++] = (char) (0xF0 | ((cp >> 18) & 0x07));
        out[(*used)++] = (char) (0x80 | ((cp >> 12) & 0x3F));
        out[(*used)++] = (char) (0x80 | ((cp >> 6) & 0x3F));
        out[(*used)++] = (char) (0x80 | (cp & 0x3F));
    }
}

bool mqtt_json_extract_string(const char *json, const char *key, char *out, size_t out_size) {
    char pattern[40];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *pos = strstr(json, pattern);
    if (!pos) {
        return false;
    }
    pos += strlen(pattern);
    while (*pos == ' ' || *pos == '\t' || *pos == '\r' || *pos == '\n') pos++;
    if (*pos != ':') return false;
    pos++;
    while (*pos == ' ' || *pos == '\t' || *pos == '\r' || *pos == '\n') pos++;
    if (*pos != '"') return false;
    pos++;

    size_t used = 0;
    while (*pos && *pos != '"' && used + 1 < out_size) {
        if (*pos == '\\' && pos[1]) {
            pos++;
            switch (*pos) {
                case 'n': out[used++] = ' '; break;
                case 'r': out[used++] = ' '; break;
                case 't': out[used++] = ' '; break;
                case 'u': {
                    uint32_t cp = 0;
                    if (read_json_hex4(pos + 1, &cp)) {
                        pos += 5;
                        if (cp >= 0xD800 && cp <= 0xDBFF && pos[0] == '\\' && pos[1] == 'u') {
                            uint32_t low = 0;
                            if (read_json_hex4(pos + 2, &low) && low >= 0xDC00 && low <= 0xDFFF) {
                                cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                                pos += 6;
                            }
                        }
                        append_utf8(out, out_size, &used, cp);
                    } else {
                        out[used++] = 'u';
                        pos++;
                    }
                    break;
                }
                case '"':
                case '\\':
                case '/':
                    out[used++] = *pos;
                    pos++;
                    break;
                default:
                    out[used++] = *pos;
                    pos++;
                    break;
            }
        } else {
            out[used++] = *pos++;
        }
    }
    out[used] = '\0';
    return used > 0;
}

bool mqtt_json_extract_int(const char *json, const char *key, int *out) {
    if (!json || !key || !out) return false;
    char pattern[40];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *pos = strstr(json, pattern);
    if (!pos) return false;
    pos += strlen(pattern);
    while (*pos == ' ' || *pos == '\t' || *pos == '\r' || *pos == '\n') pos++;
    if (*pos != ':') return false;
    pos++;
    while (*pos == ' ' || *pos == '\t' || *pos == '\r' || *pos == '\n' || *pos == '"') pos++;
    if (!((*pos >= '0' && *pos <= '9') || *pos == '-')) return false;

    *out = atoi(pos);
    return true;
}

static void write_sanitized_value(FILE *file, const char *key, const char *value) {
    fprintf(file, "%s=", key);
    for (size_t i = 0; value[i]; ++i) {
        char ch = value[i];
        fputc((ch == '\n' || ch == '\r') ? ' ' : ch, file);
    }
    fputc('\n', file);
}

static void truncate_log_if_needed(const char *path, long max_bytes) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return;
    }
    long size = ftell(file);
    fclose(file);

    if (size > max_bytes) {
        file = fopen(path, "w");
        if (file) {
            fclose(file);
        }
    }
}

bool mqtt_write_notification_files(const char *mode, const char *title, const char *message) {
    uint64_t id = mqtt_monotonic_ms();

    FILE *current = fopen(NOTIFICATION_CURRENT_PATH, "w");
    if (!current) {
        return false;
    }
    fprintf(current, "id=%llu\n", (unsigned long long) id);
    write_sanitized_value(current, "mode", mode);
    write_sanitized_value(current, "title", title);
    write_sanitized_value(current, "message", message);
    fprintf(current, "duration_ms=4500\n");
    fclose(current);

    truncate_log_if_needed(NOTIFICATION_LOG_PATH, NOTIFICATION_LOG_MAX_BYTES);
    FILE *log = fopen(NOTIFICATION_LOG_PATH, "a");
    if (log) {
        fprintf(log, "%llu mode=%s title=%s message=%s\n", (unsigned long long) id, mode, title, message);
        fclose(log);
    }
    return true;
}

static void clean_dir_recursive(const char *dir_path) {
    DIR *d = opendir(dir_path);
    if (!d) {
        return;
    }

    struct dirent *entry;
    char path_buf[512];
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        snprintf(path_buf, sizeof(path_buf), "%s/%s", dir_path, entry->d_name);
        struct stat st;
        if (stat(path_buf, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                clean_dir_recursive(path_buf);
                rmdir(path_buf);
            } else {
                remove(path_buf);
            }
        } else {
            remove(path_buf);
        }
    }
    closedir(d);
}

bool mqtt_prepare_firmware_directory(void) {
    clean_dir_recursive("sdmc:/firmware");
    mkdir("sdmc:/firmware", 0777);
    return true;
}
