#include "title_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    u64 application_id;
    char *name;
} TitleCacheEntry;

static TitleCacheEntry *g_title_entries;
static size_t g_title_entry_count;
static bool g_title_cache_loaded;

static char *duplicate_text(const char *value) {
    size_t len = strlen(value);
    char *copy = malloc(len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, value, len + 1);
    return copy;
}

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

static bool parse_title_id(const char *value, u64 *application_id) {
    if (strncasecmp(value, "0x", 2) == 0) {
        value += 2;
    }

    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 16);
    if (!end || *end != '\0' || parsed == 0) {
        return false;
    }

    *application_id = (u64) parsed;
    return true;
}

static int compare_title_entries(const void *lhs, const void *rhs) {
    const TitleCacheEntry *a = (const TitleCacheEntry *) lhs;
    const TitleCacheEntry *b = (const TitleCacheEntry *) rhs;
    if (a->application_id < b->application_id) {
        return -1;
    }
    if (a->application_id > b->application_id) {
        return 1;
    }
    return 0;
}

static bool add_title_entry(u64 application_id, const char *name) {
    TitleCacheEntry *entries = realloc(g_title_entries, (g_title_entry_count + 1) * sizeof(*g_title_entries));
    if (!entries) {
        return false;
    }

    g_title_entries = entries;
    g_title_entries[g_title_entry_count].name = duplicate_text(name);
    if (!g_title_entries[g_title_entry_count].name) {
        return false;
    }
    g_title_entries[g_title_entry_count].application_id = application_id;
    ++g_title_entry_count;
    return true;
}

static void title_cache_load(void) {
    if (g_title_cache_loaded) {
        return;
    }
    g_title_cache_loaded = true;

    FILE *file = fopen(TITLE_CACHE_PATH, "r");
    if (!file) {
        return;
    }

    char line[640];
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
        if (value[0] == '\0') {
            continue;
        }

        u64 application_id = 0;
        if (parse_title_id(key, &application_id)) {
            add_title_entry(application_id, value);
        }
    }

    fclose(file);
    qsort(g_title_entries, g_title_entry_count, sizeof(*g_title_entries), compare_title_entries);
}

bool title_cache_lookup(u64 application_id, char *out, size_t out_size) {
    if (application_id == 0 || out_size == 0) {
        return false;
    }

    title_cache_load();

    TitleCacheEntry wanted;
    wanted.application_id = application_id;
    wanted.name = NULL;
    TitleCacheEntry *entry = bsearch(&wanted, g_title_entries, g_title_entry_count, sizeof(*g_title_entries), compare_title_entries);
    if (!entry) {
        return false;
    }

    snprintf(out, out_size, "%s", entry->name);
    return true;
}
