#pragma once

#include "app_state.h"

#include <switch.h>
#include <stdbool.h>
#include <stddef.h>

#define TITLE_CACHE_PATH "sdmc:/switch/switch-ha/titles.txt"
#define TITLE_INDEX_PATH "sdmc:/switch/switch-ha/titles.idx"
#define MAX_INSTALLED_GAMES 256

typedef struct {
    u64 application_id;
    char name[128];
} InstalledGame;

void title_cache_init(void);
bool title_cache_lookup(u64 application_id, char *out, size_t out_size);
size_t title_cache_get_installed_games(InstalledGame *out_games, size_t max_games);
size_t title_cache_refresh_installed_games(void);
bool title_cache_find_application_id_by_name(const char *name, u64 *out_application_id);
bool title_cache_launch_game(AppState *state, u64 application_id);
void title_cache_build_json_options_array(char *out, size_t out_size);
