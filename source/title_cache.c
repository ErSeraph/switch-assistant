#include "title_cache.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static void trim_line(char *value) {
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

static void format_title_id(u64 application_id, char *out, size_t out_size) {
    snprintf(out, out_size, "%016llx", (unsigned long long) application_id);
}

bool title_cache_lookup(u64 application_id, char *out, size_t out_size) {
    if (application_id == 0 || out_size == 0) {
        return false;
    }

    FILE *file = fopen(TITLE_CACHE_PATH, "r");
    if (!file) {
        return false;
    }

    char wanted[24];
    format_title_id(application_id, wanted, sizeof(wanted));

    char line[640];
    bool found = false;
    while (fgets(line, sizeof(line), file)) {
        trim_line(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
            continue;
        }

        char *sep = strchr(line, ';');
        if (!sep) {
            continue;
        }
        *sep = '\0';

        char *key = line;
        char *value = sep + 1;
        trim_line(key);
        trim_line(value);
        if (strncasecmp(key, "0x", 2) == 0) {
            key += 2;
        }

        if (strcasecmp(key, wanted) == 0 && value[0] != '\0') {
            snprintf(out, out_size, "%s", value);
            found = true;
            break;
        }
    }

    fclose(file);
    return found;
}
