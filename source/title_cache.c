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

static int compare_title_entries_by_offset(const void *lhs, const void *rhs) {
    int result = compare_title_entries(lhs, rhs);
    if (result != 0) {
        return result;
    }
    const TitleIndexEntry *a = (const TitleIndexEntry *) lhs;
    const TitleIndexEntry *b = (const TitleIndexEntry *) rhs;
    return a->offset < b->offset ? -1 : a->offset > b->offset;
}

static bool parse_title_line(char *line, u64 *application_id, char **name) {
    trim_line(line);
    char *sep = strchr(line, ';');
    if (!sep) {
        return false;
    }
    *sep = '\0';

    if (!parse_title_id(line, application_id)) {
        return false;
    }

    *name = sep + 1;
    trim_line(*name);
    return (*name)[0] != '\0';
}

static bool read_title_line_at_offset(FILE *file, u64 offset, u64 *application_id, char **name, char *line, size_t line_size) {
    if (fseek(file, (long) offset, SEEK_SET) != 0 || !fgets(line, line_size, file)) {
        return false;
    }
    return parse_title_line(line, application_id, name);
}

static bool title_index_read_file(TitleIndexEntry **entries_out, size_t *count_out) {
    FILE *file = fopen(TITLE_INDEX_PATH, "rb");
    if (!file) {
        return false;
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
        return false;
    }

    TitleIndexEntry *entries = malloc((size_t) count * sizeof(*entries));
    if (!entries) {
        fclose(file);
        return false;
    }

    if (fread(entries, sizeof(*entries), count, file) != count) {
        free(entries);
        fclose(file);
        return false;
    }

    fclose(file);
    *entries_out = entries;
    *count_out = count;
    return true;
}

// The index stores byte offsets into titles.txt, so it breaks silently when the
// text file changes (edited by hand, or checked out with CRLF line endings).
static bool title_index_matches_text(const TitleIndexEntry *entries, size_t count) {
    FILE *file = fopen(TITLE_CACHE_PATH, "rb");
    if (!file) {
        return false;
    }

    const TitleIndexEntry *last_line = &entries[0];
    for (size_t i = 1; i < count; ++i) {
        if (entries[i].offset > last_line->offset) {
            last_line = &entries[i];
        }
    }

    const TitleIndexEntry *samples[] = {&entries[0], &entries[count / 2], &entries[count - 1], last_line};
    char line[640];
    u64 application_id = 0;
    char *name = NULL;
    bool ok = true;
    for (size_t i = 0; ok && i < sizeof(samples) / sizeof(samples[0]); ++i) {
        ok = read_title_line_at_offset(file, samples[i]->offset, &application_id, &name, line, sizeof(line)) &&
             application_id == samples[i]->application_id;
    }

    // Lines appended after the last indexed one are not in the index either.
    while (ok && fgets(line, sizeof(line), file)) {
        ok = !parse_title_line(line, &application_id, &name);
    }

    fclose(file);
    return ok;
}

// Collects (title id, line offset) pairs; with entries == NULL it only counts them.
static size_t title_index_scan_text(FILE *file, TitleIndexEntry *entries, size_t capacity) {
    size_t count = 0;
    char line[640];
    long offset = ftell(file);
    while (fgets(line, sizeof(line), file)) {
        long next_offset = ftell(file);
        size_t len = strlen(line);
        bool complete = len > 0 && line[len - 1] == '\n';

        u64 application_id = 0;
        char *name = NULL;
        if (parse_title_line(line, &application_id, &name)) {
            if (entries && count < capacity) {
                entries[count].application_id = application_id;
                entries[count].offset = (u64) offset;
            }
            count++;
        }

        // Skip the rest of an overlong line so the next offset starts a new line.
        while (!complete && fgets(line, sizeof(line), file)) {
            len = strlen(line);
            complete = len > 0 && line[len - 1] == '\n';
            next_offset = ftell(file);
        }
        offset = next_offset;
    }
    return count;
}

static bool title_index_build_from_text(TitleIndexEntry **entries_out, size_t *count_out) {
    FILE *file = fopen(TITLE_CACHE_PATH, "rb");
    if (!file) {
        return false;
    }

    // Two passes keep peak memory at the final index size; the sysmodule heap is small.
    size_t count = title_index_scan_text(file, NULL, 0);
    TitleIndexEntry *entries = count > 0 && count <= 100000 ? malloc(count * sizeof(*entries)) : NULL;
    if (!entries || fseek(file, 0, SEEK_SET) != 0) {
        free(entries);
        fclose(file);
        return false;
    }
    size_t found = title_index_scan_text(file, entries, count);
    fclose(file);
    if (found < count) {
        count = found;
    }
    if (count == 0) {
        free(entries);
        return false;
    }

    qsort(entries, count, sizeof(*entries), compare_title_entries_by_offset);
    size_t unique = 1;
    for (size_t i = 1; i < count; ++i) {
        if (entries[i].application_id != entries[unique - 1].application_id) {
            entries[unique++] = entries[i];
        }
    }

    *entries_out = entries;
    *count_out = unique;
    return true;
}

static void title_index_load(void) {
    if (g_title_index_loaded) {
        return;
    }
    g_title_index_loaded = true;

    TitleIndexEntry *entries = NULL;
    size_t count = 0;
    if (title_index_read_file(&entries, &count) && title_index_matches_text(entries, count)) {
        g_title_index_entries = entries;
        g_title_index_entry_count = count;
        return;
    }
    free(entries);
    entries = NULL;

    if (title_index_build_from_text(&entries, &count)) {
        g_title_index_entries = entries;
        g_title_index_entry_count = count;
    }
}

static bool read_title_name_at_offset(u64 offset, char *out, size_t out_size) {
    FILE *file = fopen(TITLE_CACHE_PATH, "rb");
    if (!file) {
        return false;
    }

    char line[640];
    u64 parsed_application_id = 0;
    char *name = NULL;
    bool ok = read_title_line_at_offset(file, offset, &parsed_application_id, &name, line, sizeof(line));
    fclose(file);
    if (!ok) {
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
