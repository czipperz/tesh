#pragma once

#include <SDL.h>
#include <SDL_ttf.h>
#include <stdint.h>
#include <cz/vector.hpp>

struct Visual_Point {
    int y;            // visual y
    int x;            // visual x
    uint64_t column;  // column number
    uint64_t outer;   // backlog id
    uint64_t inner;   // backlog-relative index
};

struct Visual_Tile {
    uint64_t outer;  // backlog id + 1, 0 = null
    uint64_t inner;  // backlog-relative index
};

enum Selection_Type {
    SELECT_DISABLED,
    SELECT_EMPTY,
    SELECT_REGION,
    SELECT_FINISHED,
};

struct Selection {
    Selection_Type type;
    Visual_Tile down, current;
    Visual_Tile start, end;
    uint32_t bg_color;
    bool expand_word : 1;
    bool expand_line : 1;
};

struct Surface_Cache {
    cz::Vector<uint32_t> code_points;
    cz::Vector<SDL_Surface*> surfaces;
};

enum Scroll_Mode {
    AUTO_PAGE,
    AUTO_SCROLL,
    MANUAL_SCROLL,
    PROMPT_SCROLL,
};

struct Backlog_State;

struct Render_State {
    TTF_Font* font;
    int font_size;
    float dpi_scale;
    int font_width;
    int font_height;
    int window_cols;
    int window_rows;
    int window_rows_ru;

    Surface_Cache caches[256];

    bool grid_is_valid;
    cz::Vector<Visual_Tile> grid;

    bool complete_redraw;

    Visual_Point backlog_start;  // First point that was drawn
    Visual_Point backlog_end;    // Last point that was drawn

    Scroll_Mode scroll_mode;

    Selection selection;

    SDL_Cursor* default_cursor;
    // SDL_Cursor * select_cursor;
    SDL_Cursor* click_cursor;

    // We want to be able to change the order to be different than the
    // order the processes were ran in.  So store the visual order here,
    // because everything that cares about it also wants the render state.
    cz::Vector<Backlog_State*> visbacklogs;
    uint64_t selected_outer;
    uint64_t attached_outer;
};

void set_icon(SDL_Window* sdl_window);

void close_font(Render_State* rend);
TTF_Font* open_font(const char* path, int font_size);

int coord_trans(Visual_Point* point, int num_cols, char ch);

bool render_code_point(SDL_Surface* window_surface,
                       Render_State* rend,
                       Visual_Point* point,
                       uint32_t background,
                       uint8_t foreground,
                       bool underline,
                       const char seq[5],
                       bool set_tile);
void resize_font(int font_size, Render_State* rend);

size_t find_visbacklog(Render_State* rend, uint64_t the_id);
