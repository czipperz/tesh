#include "render.hpp"

#include <SDL_image.h>
#include <Tracy.hpp>
#include <cz/format.hpp>
#include <cz/string.hpp>

#include "config.hpp"
#include "global.hpp"

#ifdef _WIN32
#include <SDL_syswm.h>
#endif

void set_icon(SDL_Window* sdl_window) {
    ZoneScoped;

    // Try to set logo using Windows magic.  This results in
    // much higher definition on Windows so is preferred.
#ifdef _WIN32
    SDL_SysWMinfo wminfo;
    SDL_VERSION(&wminfo.version);
    if (SDL_GetWindowWMInfo(sdl_window, &wminfo) == 1) {
        HWND hwnd = wminfo.info.win.window;

        HINSTANCE handle = GetModuleHandle(nullptr);
        HICON icon = LoadIcon(handle, "IDI_MAIN_ICON");
        if (icon) {
            SetClassLongPtr(hwnd, GCLP_HICON, (LONG_PTR)icon);
            return;
        }
    }
#endif

    // Fallback to letting SDL do it.
    cz::String logo = cz::format(temp_allocator, program_directory, "logo.png");
    SDL_Surface* icon = IMG_Load(logo.buffer);
    if (icon) {
        SDL_SetWindowIcon(sdl_window, icon);
        SDL_FreeSurface(icon);
    }
}

void close_font(Render_State* rend) {
    ZoneScoped;
    for (int i = 0; i < CZ_DIM(rend->backlog_cache); i++) {
        SDL_FreeSurface(rend->backlog_cache[i]);
        rend->backlog_cache[i]   = NULL;
    }
    for (int i = 0; i < CZ_DIM(rend->prompt_cache); i++) {
        SDL_FreeSurface(rend->prompt_cache[i]);
        rend->prompt_cache[i] = NULL;
    }
    for (int i = 0; i < CZ_DIM(rend->selection_cache); i++) {
        SDL_FreeSurface(rend->selection_cache[i]);
        rend->selection_cache[i] = NULL;
    }
    for (int i = 0; i < CZ_DIM(rend->directory_cache); i++) {
        SDL_FreeSurface(rend->directory_cache[i]);
        rend->directory_cache[i] = NULL;
    }
    TTF_CloseFont(rend->font);
}

TTF_Font* open_font(const char* path, int font_size) {
    ZoneScoped;
    return TTF_OpenFont(path, font_size);
}

static SDL_Surface* rasterize_character(const char* text,
                                        TTF_Font* font,
                                        int style,
                                        SDL_Color fgc) {
    ZoneScoped;
    TTF_SetFontStyle(font, style);
    return TTF_RenderText_Blended(font, text, fgc);
}

static SDL_Surface* rasterize_character_cached(Render_State* rend,
                                               SDL_Surface** cache,
                                               char ch,
                                               SDL_Color color) {
    uint8_t index = (uint8_t)ch;
    if (cache[index])
        return cache[index];
    if (ch == '\0')
        ch = ' ';
    char text[2] = {ch, 0};
    SDL_Surface* surface = rasterize_character(text, rend->font, 0, color);
    CZ_ASSERT(surface);
    cache[index] = surface;
    return surface;
}

int coord_trans(Visual_Point* point, int num_cols, char ch) {
    ++point->inner;

    if (ch == '\n') {
        ++point->y;
        point->x = 0;
        point->column = 0;
        return 0;
    }

    int width = 1;
    if (ch == '\t') {
        uint64_t lcol2 = point->column;
        lcol2 += cfg.tab_width;
        lcol2 -= lcol2 % cfg.tab_width;
        width = (int)(lcol2 - point->column);
    }

    // TODO: should this be >=?
    if (point->x + width > num_cols) {
        ++point->y;
        point->x = 0;
    }

    point->x += width;
    point->column += width;
    return width;
}

bool render_char(SDL_Surface* window_surface,
                 Render_State* rend,
                 Visual_Point* point,
                 SDL_Surface** cache,
                 uint32_t background,
                 SDL_Color foreground,
                 char c,
                 bool set_tile) {
    if (set_tile) {
        size_t index = point->y * rend->window_cols + point->x;
        if (index < rend->grid.len) {
            Visual_Tile* tile = &rend->grid[index];
            tile->outer = point->outer + 1;
            tile->inner = point->inner;

            if (c == '\n') {
                for (int x = point->x; x < rend->window_cols; ++x) {
                    ++index;
                    ++tile;
                    tile->outer = point->outer + 1;
                    tile->inner = point->inner;
                }
            }
        }
    }

    if (rend->selection.state == SELECT_REGION || rend->selection.state == SELECT_FINISHED) {
        bool inside_start = ((point->outer > rend->selection.start.outer - 1) ||
                             (point->outer == rend->selection.start.outer - 1 &&
                              point->inner >= rend->selection.start.inner));
        bool inside_end = ((point->outer < rend->selection.end.outer - 1) ||
                           (point->outer == rend->selection.end.outer - 1 &&
                            point->inner <= rend->selection.end.inner));
        if (inside_start && inside_end) {
            cache = rend->selection_cache;
            foreground = rend->selection_fg_color;
            background = rend->selection.bg_color;
        }
    }

    SDL_Rect rect = {point->x * rend->font_width, point->y * rend->font_height};
    uint64_t old_y = point->y;
    int width = coord_trans(point, rend->window_cols, c);

    if (point->y != old_y) {
        rect.w = window_surface->w - rect.x;
        rect.h = rend->font_height;
        SDL_FillRect(window_surface, &rect, background);

        rect.x = 0;
        rect.y += rend->font_height;

        // Beyond bottom of screen.
        if (point->y >= rend->window_rows_ru)
            return false;

        // Newlines aren't drawn.
        if (width == 0)
            return true;
    }

    ZoneScopedN("blit_character");
    if (c == '\t') {
        rect.w = width * rend->font_width;
        rect.h = rend->font_height;
        SDL_FillRect(window_surface, &rect, background);
    } else {
        SDL_Surface* s = rasterize_character_cached(rend, cache, c, foreground);
        rect.w = rend->font_width;
        rect.h = rend->font_height;
        SDL_FillRect(window_surface, &rect, background);
        SDL_BlitSurface(s, NULL, window_surface, &rect);
    }

    return true;
}
