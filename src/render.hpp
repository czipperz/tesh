#pragma once

#include <SDL.h>
#include <SDL_ttf.h>
#include <stdint.h>

struct Visual_Point {
    int y;            // visual y
    int x;            // visual x
    uint64_t index;   // absolute position
    uint64_t line;    // line number
    uint64_t column;  // column number
};

struct Render_State {
    TTF_Font* font;
    int font_size;
    float dpi_scale;
    int font_width;
    int font_height;
    int window_cols;
    int window_rows;
    int window_rows_ru;
    SDL_Surface* backlog_cache[256];
    SDL_Surface* prompt_cache[256];

    bool complete_redraw;

    SDL_Color backlog_fg_color;
    Visual_Point backlog_start;  // First point that was drawn
    Visual_Point backlog_end;    // Last point that was drawn

    bool auto_page;
    bool auto_scroll;

    SDL_Color prompt_fg_color;
};


void set_icon(SDL_Window* sdl_window);

void close_font(Render_State* rend);
TTF_Font* open_font(const char* path, int font_size);

int coord_trans(Visual_Point* point, int num_cols, char ch);

bool render_char(SDL_Surface* window_surface,
                 Render_State* rend,
                 Visual_Point* point,
                 SDL_Surface** cache,
                 uint32_t background,
                 SDL_Color foreground,
                 char c);
