#include "title_cache.h"
#include "logger.h"

#include <switch.h>
#include <switch/services/ns.h>
#include <switch/nacp.h>
#include <switch/services/applet.h>


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

static InstalledGame g_installed_games[MAX_INSTALLED_GAMES];
static size_t g_installed_game_count = 0;

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

static const char *case_str_search(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    if (needle[0] == '\0') return haystack;
    size_t needle_len = strlen(needle);
    for (size_t i = 0; haystack[i]; ++i) {
        if (strncasecmp(haystack + i, needle, needle_len) == 0) {
            return haystack + i;
        }
    }
    return NULL;
}

static bool parse_title_id(const char *value, u64 *application_id) {
    if (!value || value[0] == '\0') {
        return false;
    }

    while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') {
        value++;
    }

    if (strncasecmp(value, "0x", 2) == 0) {
        value += 2;
    }

    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 16);
    if (!end || (*end != '\0' && *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n') || parsed == 0) {
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

static int compare_installed_games(const void *lhs, const void *rhs) {
    const InstalledGame *a = (const InstalledGame *) lhs;
    const InstalledGame *b = (const InstalledGame *) rhs;
    return strcasecmp(a->name, b->name);
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

void title_cache_init(void) {
    title_index_load();
    title_cache_refresh_installed_games();
}

size_t title_cache_get_installed_games(InstalledGame *out_games, size_t max_games) {
    if (g_installed_game_count == 0) {
        title_cache_refresh_installed_games();
    }
    if (out_games && max_games > 0) {
        size_t copy_count = g_installed_game_count < max_games ? g_installed_game_count : max_games;
        memcpy(out_games, g_installed_games, copy_count * sizeof(InstalledGame));
        return copy_count;
    }
    return g_installed_game_count;
}

int title_cache_get_max_players(u64 application_id, const char *name) {
    FILE *override = fopen("sdmc:/switch/switch-ha/players.ini", "r");
    if (!override) {
        override = fopen("sdmc:/switch/switch-ha/players.txt", "r");
    }
    if (override) {
        char line[256];
        while (fgets(line, sizeof(line), override)) {
            trim_line(line);
            char *sep = strchr(line, '=');
            if (!sep) sep = strchr(line, ';');
            if (!sep) continue;
            *sep = '\0';
            const char *key = line;
            const char *val = sep + 1;
            u64 parsed_id = 0;
            if (parse_title_id(key, &parsed_id) && parsed_id == application_id) {
                int p = atoi(val);
                if (p > 0) {
                    fclose(override);
                    return p;
                }
            }
            if (name && (strcasecmp(key, name) == 0 || case_str_search(name, key) != NULL)) {
                int p = atoi(val);
                if (p > 0) {
                    fclose(override);
                    return p;
                }
            }
        }
        fclose(override);
    }

    if (!name || name[0] == '\0') {
        return 1;
    }

    const char *games_8p[] = {
        "Smash Bros", "Super Smash", "Bomberman", "Super Bomberman", "1-2-Switch",
        "1-2 Switch", "Everybody 1-2", "Runbow", "Knight Squad", "Killer Queen",
        "TowerFall", "Tower Fall", "Pico Park", "PicoPark", "WarioWare", "Gang Beasts", "Just Dance",
        "Virtua Tennis", "Mario Strikers", "Super Mario Strikers", "Death Squared",
        "Aeolis Tournament", "Bulanci", "Rover Wars", "Sky Gamblers", "Splatoon",
        "Shredder's Revenge", "Party Golf", "ClusterPuck", "8 Player", "8-Player", "8 Players",
        "Super Bomberman R", "Luigi's Mansion 3", "Super Smash Bros. Ultimate", "Runbow Deluxe"
    };
    for (size_t i = 0; i < sizeof(games_8p) / sizeof(games_8p[0]); ++i) {
        if (case_str_search(name, games_8p[i]) != NULL) {
            return 8;
        }
    }

    const char *games_4p[] = {
        "Mario Party", "Mario Tennis", "Mario Golf", "Mario Kart", "Overcooked",
        "Moving Out", "Castle Crashers", "TMNT", "Teenage Mutant", "Streets of Rage",
        "Brawlhalla", "Rocket League", "Minecraft", "Rayman", "Borderlands",
        "Diablo", "Sonic", "FIFA", "EA SPORTS", "FC 24", "FC 25", "NBA",
        "WWE", "NFL", "Jackbox", "Stick Fight", "Boomerang Fu", "Worms", "SpeedRunners",
        "Ultimate Chicken", "Trine", "Heave Ho", "Hidden in Plain", "Lego",
        "Monopoly", "Uno", "Trivial Pursuit", "Carnival Games", "Switch Sports",
        "Nintendo Switch Sports", "Party", "Strikers", "Super Mario 3D World",
        "Kirby Star Allies", "Kirby", "Brawl", "SpongeBob", "Flat Heroes",
        "Rubber Bandits", "Spelunky", "Risk of Rain", "Lovers in a Dangerous",
        "Animal Bomber", "NUMAN ATHLETICS", "THE TIN STAR", "Black and White Bushido",
        "BOMBFEST", "Chalk Dash Carnival", "Chapeau", "Circuit Superstars",
        "Cube Creator X", "Doggie Ninja", "FUN! FUN! Animal Park", "Horizon Chase",
        "HYPERCHARGE", "Inspector Gadget", "Jamestown", "JDM Racing", "Jujutsu Kaisen",
        "Moorhuhn Kart", "MY HERO ONE'S JUSTICE", "NBA Playgrounds", "NCL: USA Bowl",
        "Pixel Family Fun", "Pocket Mini Golf", "Professor Rubik", "Road to Ballhalla",
        "Run Box Run", "Simple Mini Golf", "Slots of Poker", "Super Korotama",
        "Super Tennis Blast", "SuperDuck", "Them Bombs", "Theme Park Jam",
        "This Means Warp", "Tokyo Cooking", "Unruly Heroes", "Wild Card Football",
        "Winter Sports Games", "Wizards Tourney", "Worlds of Magic", "Broforce",
        "Tools Up", "Conduct Together", "Unrailed", "For the King", "Magicka",
        "Nine Parchments", "Full Metal Furies", "Gauntlet", "Marvel Ultimate Alliance",
        "Cat Quest", "River City", "Scott Pilgrim", "Capcom Beat", "Alien Hominid",
        "Crawl", "Screencheat", "Astro Bears", "Timberman", "Duck Game", "Nidhogg",
        "Lethal League", "Marooners", "Party Panic", "Cake Bash", "Air Hockey",
        "Mini Golf", "Bowling", "Darts", "Billiards", "3 Card Poker", "Kart",
        "4 Player", "4-Player", "4 Players", "Olympic Games", "Formula Racing",
        "Street Fighter 30th", "Astro Bot", "TowerFall Ascension", "Puyo Puyo",
        "Tetris Effect", "Super Monkey Ball", "Golf With Your Friends", "Death Road to Canada"
    };
    for (size_t i = 0; i < sizeof(games_4p) / sizeof(games_4p[0]); ++i) {
        if (case_str_search(name, games_4p[i]) != NULL) {
            return 4;
        }
    }

    const char *games_2p[] = {
        "Odyssey", "Super Mario Odyssey", "Snipperclips", "Cuphead", "It Takes Two",
        "Unravel", "Portal 2", "Street Fighter", "Mortal Kombat", "Dragon Ball",
        "Tekken", "Guilty Gear", "King of Fighters", "Skullgirls", "Arms",
        "Pokemon", "Pokémon", "Pokken", "Pokkén", "Luigi's Mansion", "Pikmin", "Donkey Kong",
        "Tropical Freeze", "Captain Toad", "Warriors", "Hyrule", "Fire Emblem",
        "Children of Morta", "Haven", "A Way Out", "Gungeon",
        "Guacamelee", "Terraria", "Untitled Goose", "Duel", "Versus", " VS ", " vs ",
        "Bros", "Fighter", "Fighting", "Rumble", "Coop", "Co-op", "Team", "Two", "2 Player",
        "2-Player", "2 Players", "AngerForce", "LIGHTNING FIGHTERS", "Brain Meltdown",
        "Darius Cozmic", "Dodgeball Academia", "Double Dragon", "Fightro", "Forklift Extreme",
        "Galactic Wars", "GensokyoDefenders", "I Am The Hero", "Johnny Turbo", "Lesson Learned",
        "LET'S THROOOW", "Musashi vs Cthulhu", "Ninjin", "QUByte Classics", "Rad Rodgers",
        "Rage of the Dragons", "Rawr-Off", "Redgar", "SEA POWER", "Spirit Arena",
        "Super Dodgeball Beats", "Twin Robots", "Uragun", "Shovel Knight", "Metal Slug",
        "Contra", "Mega Man", "Hotline Miami", "Binding of Isaac", "Salt and Sanctuary",
        "Wizard of Legend", "Battle", "Boxing", "Wrestling", "Tennis", "Chess", "Checkers",
        "A Hole New World", "Basketball Anime Girls", "Bubble Trouble", "Drunken Fist",
        "Dunk Trickster", "Morsels", "Piczle Colors", "Pid", "PUTRID SHOT", "Rally Racer",
        "Rocket Knight", "Sail Forth", "Shotgun King", "Beach Boys", "Tabletop", "Ping Pong",
        "Air Hockey 3D", "3D Air Hockey", "Football", "Soccer", "Basketball"
    };
    for (size_t i = 0; i < sizeof(games_2p) / sizeof(games_2p[0]); ++i) {
        if (case_str_search(name, games_2p[i]) != NULL) {
            return 2;
        }
    }

    return 1;
}

size_t title_cache_refresh_installed_games(void) {
    title_index_load();

    size_t found = 0;
    Result rc = nsInitialize();
    if (R_SUCCEEDED(rc)) {
        s32 offset = 0;
        NsApplicationRecord records[32];
        s32 out_count = 0;

        NsApplicationControlData *ctrl_data = malloc(sizeof(NsApplicationControlData));

        while (found < MAX_INSTALLED_GAMES) {
            s32 fetch_count = (MAX_INSTALLED_GAMES - found > 32) ? 32 : (s32)(MAX_INSTALLED_GAMES - found);
            Result list_rc = nsListApplicationRecord(records, fetch_count, offset, &out_count);
            if (R_FAILED(list_rc) || out_count <= 0) {
                break;
            }

            for (s32 i = 0; i < out_count && found < MAX_INSTALLED_GAMES; ++i) {
                u64 app_id = records[i].application_id;
                if (app_id == 0) {
                    continue;
                }

                char name[128] = "";
                if (!title_cache_lookup(app_id, name, sizeof(name))) {
                    if (ctrl_data) {
                        size_t actual_size = 0;
                        if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage, app_id, ctrl_data, sizeof(*ctrl_data), &actual_size))) {
                            NacpLanguageEntry *lang_entry = NULL;
                            if (R_SUCCEEDED(nacpGetLanguageEntry(&ctrl_data->nacp, &lang_entry)) && lang_entry && lang_entry->name[0] != '\0') {
                                size_t lang_name_len = strnlen(lang_entry->name, sizeof(name) - 1);
                                memcpy(name, lang_entry->name, lang_name_len);
                                name[lang_name_len] = '\0';
                            }
                        }
                    }
                }

                if (name[0] == '\0') {
                    snprintf(name, sizeof(name), "0x%016llX", (unsigned long long) app_id);
                }
                trim_line(name);

                // Avoid duplicates
                bool dup = false;
                for (size_t d = 0; d < found; ++d) {
                    if (g_installed_games[d].application_id == app_id) {
                        dup = true;
                        break;
                    }
                }
                if (!dup) {
                    g_installed_games[found].application_id = app_id;
                    size_t name_len = strlen(name);
                    if (name_len >= sizeof(g_installed_games[found].name)) {
                        name_len = sizeof(g_installed_games[found].name) - 1;
                    }
                    memcpy(g_installed_games[found].name, name, name_len);
                    g_installed_games[found].name[name_len] = '\0';
                    g_installed_games[found].max_players = title_cache_get_max_players(app_id, name);
                    found++;
                }
            }
            offset += out_count;
        }

        if (ctrl_data) {
            free(ctrl_data);
        }
        nsExit();
    }

    // If NS service didn't find any games or failed, populate from titles.txt
    if (found == 0) {
        FILE *file = fopen(TITLE_CACHE_PATH, "r");
        if (file) {
            char line[640];
            while (fgets(line, sizeof(line), file) && found < MAX_INSTALLED_GAMES) {
                trim_line(line);
                char *sep = strchr(line, ';');
                if (!sep) continue;
                *sep = '\0';
                u64 app_id = 0;
                if (!parse_title_id(line, &app_id)) continue;
                char *name = sep + 1;
                char *players_str = strchr(name, ';');
                int max_p = 0;
                if (players_str) {
                    *players_str = '\0';
                    max_p = atoi(players_str + 1);
                }
                trim_line(name);
                if (name[0] == '\0') continue;

                g_installed_games[found].application_id = app_id;
                snprintf(g_installed_games[found].name, sizeof(g_installed_games[found].name), "%s", name);
                g_installed_games[found].max_players = max_p > 0 ? max_p : title_cache_get_max_players(app_id, name);
                found++;
            }
            fclose(file);
        }
    }

    if (found > 0) {
        qsort(g_installed_games, found, sizeof(InstalledGame), compare_installed_games);
        g_installed_game_count = found;
    }

    return g_installed_game_count;
}

static void append_json_escaped_string(char *dst, size_t dst_size, size_t *pos, const char *src) {
    if (*pos + 1 >= dst_size) return;
    dst[(*pos)++] = '"';
    for (size_t i = 0; src[i] && *pos + 3 < dst_size; ++i) {
        char ch = src[i];
        if (ch == '"') {
            dst[(*pos)++] = '\\';
            dst[(*pos)++] = '"';
        } else if (ch == '\\') {
            dst[(*pos)++] = '\\';
            dst[(*pos)++] = '\\';
        } else if (ch == '\r' || ch == '\n' || ch == '\t') {
            dst[(*pos)++] = ' ';
        } else if ((unsigned char)ch >= 0x20) {
            dst[(*pos)++] = ch;
        }
    }
    if (*pos + 1 < dst_size) {
        dst[(*pos)++] = '"';
    }
}

void title_cache_build_json_options_array_for_players(char *out, size_t out_size, int min_players) {
    if (!out || out_size < 3) {
        if (out && out_size > 0) out[0] = '\0';
        return;
    }

    if (g_installed_game_count == 0) {
        title_cache_refresh_installed_games();
    }

    size_t pos = 0;
    out[pos++] = '[';

    append_json_escaped_string(out, out_size, &pos, "Home Menu");

    for (size_t i = 0; i < g_installed_game_count && pos + 4 < out_size; ++i) {
        if (g_installed_games[i].name[0] == '\0') {
            continue;
        }
        if (min_players > 1 && g_installed_games[i].max_players < min_players) {
            continue;
        }
        if (pos + 1 < out_size) {
            out[pos++] = ',';
        }
        append_json_escaped_string(out, out_size, &pos, g_installed_games[i].name);
    }

    if (pos + 1 < out_size) {
        out[pos++] = ']';
    } else {
        out[out_size - 2] = ']';
        pos = out_size - 1;
    }
    out[pos] = '\0';
}

void title_cache_build_json_options_array(char *out, size_t out_size) {
    title_cache_build_json_options_array_for_players(out, out_size, 1);
}

static void title_cache_build_json_games_list_for_attr(char *out, size_t out_size, int min_players) {
    if (!out || out_size < 3) {
        if (out && out_size > 0) out[0] = '\0';
        return;
    }

    if (g_installed_game_count == 0) {
        title_cache_refresh_installed_games();
    }

    size_t pos = 0;
    out[pos++] = '[';

    bool first = true;
    for (size_t i = 0; i < g_installed_game_count && pos + 4 < out_size; ++i) {
        if (g_installed_games[i].name[0] == '\0') {
            continue;
        }
        if (min_players > 1 && g_installed_games[i].max_players < min_players) {
            continue;
        }
        if (!first && pos + 1 < out_size) {
            out[pos++] = ',';
        }
        first = false;
        append_json_escaped_string(out, out_size, &pos, g_installed_games[i].name);
    }

    if (pos + 1 < out_size) {
        out[pos++] = ']';
    } else {
        out[out_size - 2] = ']';
        pos = out_size - 1;
    }
    out[pos] = '\0';
}

size_t title_cache_get_games_by_min_players(InstalledGame *out_games, size_t max_games, int min_players) {
    if (g_installed_game_count == 0) {
        title_cache_refresh_installed_games();
    }
    size_t count = 0;
    for (size_t i = 0; i < g_installed_game_count && (!out_games || count < max_games); ++i) {
        if (g_installed_games[i].max_players >= min_players) {
            if (out_games && count < max_games) {
                out_games[count] = g_installed_games[i];
            }
            count++;
        }
    }
    return count;
}

void title_cache_build_json_games_attr(char *out, size_t out_size, int min_players) {
    if (!out || out_size < 3) {
        if (out && out_size > 0) out[0] = '\0';
        return;
    }

    if (g_installed_game_count == 0) {
        title_cache_refresh_installed_games();
    }

    size_t count = 0;
    for (size_t i = 0; i < g_installed_game_count; ++i) {
        if (g_installed_games[i].max_players >= min_players) {
            count++;
        }
    }

    char *games_json = malloc(8192);
    if (!games_json) {
        snprintf(out, out_size, "{\"count\":%zu,\"min_players\":%d,\"games\":[]}", count, min_players);
        return;
    }
    title_cache_build_json_games_list_for_attr(games_json, 8192, min_players);

    snprintf(out, out_size, "{\"count\":%zu,\"min_players\":%d,\"games\":%s}", count, min_players, games_json);
    free(games_json);
}

bool title_cache_find_application_id_by_name(const char *name, u64 *out_application_id) {
    if (!name || name[0] == '\0' || !out_application_id) {
        return false;
    }

    char query[256];
    snprintf(query, sizeof(query), "%s", name);
    trim_line(query);

    if (strcasecmp(query, "Home Menu") == 0 || strcasecmp(query, "Home") == 0 ||
        strcasecmp(query, "None") == 0 || strcasecmp(query, "none") == 0 ||
        strcasecmp(query, "off") == 0 || strcasecmp(query, "idle") == 0) {
        return false;
    }

    // Try parsing as raw hex title ID
    u64 direct_id = 0;
    if (parse_title_id(query, &direct_id)) {
        *out_application_id = direct_id;
        return true;
    }

    if (g_installed_game_count == 0) {
        title_cache_refresh_installed_games();
    }

    // 1. Exact case-insensitive match against installed games
    for (size_t i = 0; i < g_installed_game_count; ++i) {
        if (strcasecmp(g_installed_games[i].name, query) == 0) {
            *out_application_id = g_installed_games[i].application_id;
            return true;
        }
    }

    // 2. Prefix / containment match against installed games
    for (size_t i = 0; i < g_installed_game_count; ++i) {
        if (case_str_search(query, g_installed_games[i].name) != NULL ||
            case_str_search(g_installed_games[i].name, query) != NULL) {
            *out_application_id = g_installed_games[i].application_id;
            return true;
        }
    }

    // 3. Search in titles.txt database
    FILE *file = fopen(TITLE_CACHE_PATH, "r");
    if (file) {
        char line[640];
        while (fgets(line, sizeof(line), file)) {
            trim_line(line);
            char *sep = strchr(line, ';');
            if (!sep) continue;
            *sep = '\0';
            const char *title_name = sep + 1;
            if (strcasecmp(title_name, query) == 0 || case_str_search(title_name, query) != NULL) {
                u64 app_id = 0;
                if (parse_title_id(line, &app_id)) {
                    *out_application_id = app_id;
                    fclose(file);
                    return true;
                }
            }
        }
        fclose(file);
    }

    return false;
}

bool title_cache_launch_game(AppState *state, u64 application_id) {
    if (application_id == 0) {
        return false;
    }

    char name[128] = "";
    if (!title_cache_lookup(application_id, name, sizeof(name))) {
        snprintf(name, sizeof(name), "0x%016llX", (unsigned long long) application_id);
    }

    app_state_push_log(state, "Launching game: %s (0x%016llX)", name, (unsigned long long) application_id);

    Result rc = appletRequestLaunchApplication(application_id, NULL);
    if (R_FAILED(rc)) {
        Result init_rc = appletInitialize();
        if (R_SUCCEEDED(init_rc)) {
            rc = appletRequestLaunchApplication(application_id, NULL);
            appletExit();
        }
    }

    if (R_SUCCEEDED(rc)) {
        app_state_push_log(state, "Game launch requested: %s", name);
        if (state) {
            mutexLock(&state->lock);
            state->sensor_publish_requested = true;
            mutexUnlock(&state->lock);
        }
        return true;
    } else {
        app_state_push_log(state, "Game launch failed: rc=0x%x", rc);
        return false;
    }
}
