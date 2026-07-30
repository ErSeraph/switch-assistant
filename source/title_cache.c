#include "title_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    u64 application_id;
    u64 offset;
} TitleIndexEntry;

static TitleIndexEntry *g_title_index_entries;
static size_t g_title_index_entry_count;
static bool g_title_index_loaded;
static u64 g_cached_application_id;
static char g_cached_name[0x200];

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
    const TitleIndexEntry *a = (const TitleIndexEntry *) lhs;
    const TitleIndexEntry *b = (const TitleIndexEntry *) rhs;
    if (a->application_id < b->application_id) {
        return -1;
    }
    if (a->application_id > b->application_id) {
        return 1;
    }
    return 0;
}

static void title_index_load(void) {
    if (g_title_index_loaded) {
        return;
    }
    g_title_index_loaded = true;

    FILE *file = fopen(TITLE_INDEX_PATH, "rb");
    if (!file) {
        return;
    }

    unsigned char magic[8];
    u32 version = 0;
    u32 count = 0;
    if (fread(magic, 1, sizeof(magic), file) != sizeof(magic) ||
        fread(&version, sizeof(version), 1, file) != 1 ||
        fread(&count, sizeof(count), 1, file) != 1 ||
        memcmp(magic, "SHAIDX1", 7) != 0 ||
        magic[7] != '\0' ||
        version != 1 ||
        count == 0 ||
        count > 100000) {
        fclose(file);
        return;
    }

    TitleIndexEntry *entries = malloc((size_t) count * sizeof(*entries));
    if (!entries) {
        fclose(file);
        return;
    }

    if (fread(entries, sizeof(*entries), count, file) != count) {
        free(entries);
        fclose(file);
        return;
    }

    fclose(file);
    g_title_index_entries = entries;
    g_title_index_entry_count = count;
}

static bool read_title_name_at_offset(u64 offset, char *out, size_t out_size) {
    FILE *file = fopen(TITLE_CACHE_PATH, "r");
    if (!file) {
        return false;
    }

    if (fseek(file, (long) offset, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }

    char line[640];
    if (!fgets(line, sizeof(line), file)) {
        fclose(file);
        return false;
    }
    fclose(file);

    trim_line(line);
    char *sep = strchr(line, ';');
    if (!sep) {
        return false;
    }
    *sep = '\0';

    u64 parsed_application_id = 0;
    if (!parse_title_id(line, &parsed_application_id)) {
        return false;
    }

    char *name = sep + 1;
    trim_line(name);
    if (name[0] == '\0') {
        return false;
    }

    snprintf(g_cached_name, sizeof(g_cached_name), "%s", name);
    g_cached_application_id = parsed_application_id;
    snprintf(out, out_size, "%s", g_cached_name);
    return true;
}

bool title_cache_lookup(u64 application_id, char *out, size_t out_size) {
    if (application_id == 0 || out_size == 0) {
        return false;
    }

    if (g_cached_application_id == application_id && g_cached_name[0] != '\0') {
        snprintf(out, out_size, "%s", g_cached_name);
        return true;
    }

    title_index_load();
    if (!g_title_index_entries || g_title_index_entry_count == 0) {
        return false;
    }

    TitleIndexEntry wanted;
    wanted.application_id = application_id;
    wanted.offset = 0;
    TitleIndexEntry *entry = bsearch(&wanted, g_title_index_entries, g_title_index_entry_count, sizeof(*g_title_index_entries), compare_title_entries);
    if (!entry) {
        return false;
    }

    return read_title_name_at_offset(entry->offset, out, out_size) && g_cached_application_id == application_id;
}
