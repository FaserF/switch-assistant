#include "title_cache.h"

#include <switch.h>
#include <switch/services/ns.h>
#include <switch/services/nacp.h>
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
                                snprintf(name, sizeof(name), "%s", lang_entry->name);
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
                    snprintf(g_installed_games[found].name, sizeof(g_installed_games[found].name), "%s", name);
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
                trim_line(name);
                if (name[0] == '\0') continue;

                g_installed_games[found].application_id = app_id;
                snprintf(g_installed_games[found].name, sizeof(g_installed_games[found].name), "%s", name);
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

bool title_cache_find_application_id_by_name(const char *name, u64 *out_application_id) {
    if (!name || name[0] == '\0' || !out_application_id) {
        return false;
    }

    char query[256];
    snprintf(query, sizeof(query), "%s", name);
    trim_line(query);

    if (strcasecmp(query, "None") == 0 || strcasecmp(query, "none") == 0 ||
        strcasecmp(query, "Home") == 0 || strcasecmp(query, "home") == 0 ||
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

void title_cache_build_json_options_array(char *out, size_t out_size) {
    if (!out || out_size < 3) {
        if (out && out_size > 0) out[0] = '\0';
        return;
    }

    if (g_installed_game_count == 0) {
        title_cache_refresh_installed_games();
    }

    size_t pos = 0;
    out[pos++] = '[';

    append_json_escaped_string(out, out_size, &pos, "None");

    for (size_t i = 0; i < g_installed_game_count && pos + 4 < out_size; ++i) {
        if (g_installed_games[i].name[0] == '\0') {
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
