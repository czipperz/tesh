#pragma once

#include <SDL.h>
#include <cz/slice.hpp>

struct Config_State {
    bool on_spawn_attach;
    bool on_spawn_auto_page;
    bool on_spawn_auto_scroll;
    const char* font_path;
    int default_font_size;
    int tab_width;
    uint64_t max_length;

    /// 0 = absolute minimum, 1 = compromise, 2 = everything builtin.
    int builtin_level;

    // RGB colors.
    cz::Slice<SDL_Color> process_colors;
    SDL_Color selection_bg_color;

    // 256 colors.
    const SDL_Color* theme;  // [256]
    uint8_t backlog_fg_color;
    uint8_t prompt_fg_color;
    uint8_t info_fg_color;
    uint8_t selection_fg_color;
};

extern Config_State cfg;
