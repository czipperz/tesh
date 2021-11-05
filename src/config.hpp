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
    cz::Slice<SDL_Color> process_colors;
};

extern Config_State cfg;
