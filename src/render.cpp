#include "render.hpp"

#include <SDL_image.h>
#include <Tracy.hpp>
#include <cz/binary_search.hpp>
#include <cz/format.hpp>
#include <cz/string.hpp>

#include "backlog.hpp"
#include "config.hpp"
#include "global.hpp"
#include "unicode.hpp"

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
    for (int ch = 0; ch < CZ_DIM(rend->caches); ch++) {
        Surface_Cache* cache = &rend->caches[ch];
        for (int i = 0; i < cache->surfaces.len; i++) {
            SDL_Surface** surface = &cache->surfaces.get(i);
            SDL_FreeSurface(*surface);
            *surface = NULL;
        }
        cache->code_points.len = 0;
        cache->surfaces.len = 0;
    }
    TTF_CloseFont(rend->font);
}

static SDL_Surface* rasterize_code_point(const char* text,
                                         TTF_Font* font,
                                         int style,
                                         SDL_Color fgc) {
    ZoneScoped;
    TTF_SetFontStyle(font, style);
    return TTF_RenderUTF8_Blended(font, text, fgc);
}

static SDL_Surface* rasterize_code_point_cached(Render_State* rend,
                                                const char seq[5],
                                                uint8_t color256) {
    uint32_t code_point = unicode::utf8_code_point((const uint8_t*)seq);

    // Check the cache.
    Surface_Cache* cache = &rend->caches[color256];
    size_t index;
    if (cz::binary_search(cache->code_points.as_slice(), code_point, &index))
        return cache->surfaces[index];  // Cache hit.

    // Cache miss.  Rasterize and add to the cache.
    SDL_Surface* surface = rasterize_code_point(seq, rend->font, 0, cfg.theme[color256]);

    // I've seen this case actually come up before so re-render as an invalid character.
    if (!surface) {
        if (!strcmp(seq, "\1"))
            CZ_PANIC("Failed to render");
        return rasterize_code_point_cached(rend, "\1", color256);
    }

    cache->code_points.reserve(cz::heap_allocator(), 1);
    cache->code_points.insert(index, code_point);
    cache->surfaces.reserve(cz::heap_allocator(), 1);
    cache->surfaces.insert(index, surface);

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

bool render_code_point(SDL_Surface* window_surface,
                       Render_State* rend,
                       Visual_Point* point,
                       uint32_t background,
                       uint8_t foreground,
                       bool underline,
                       const char seq[5],
                       bool set_tile) {
    if (set_tile) {
        size_t index = point->y * rend->window_cols + point->x;
        if (index < rend->grid.len) {
            Visual_Tile* tile = &rend->grid[index];
            tile->outer = point->outer + 1;
            tile->inner = point->inner;

            if (seq[0] == '\n') {
                for (int x = point->x + 1; x < rend->window_cols; ++x) {
                    ++tile;
                    tile->outer = point->outer + 1;
                    tile->inner = point->inner;
                }
            } else if (seq[0] == '\t') {
                uint64_t lcol2 = point->column;
                lcol2 += cfg.tab_width;
                lcol2 -= lcol2 % cfg.tab_width;
                uint64_t width = lcol2 - point->column;
                for (uint64_t i = 1; i < width; ++i) {
                    ++tile;
                    tile->outer = point->outer + 1;
                    tile->inner = point->inner;
                }
            }
        }

        if (rend->selection.type == SELECT_REGION || rend->selection.type == SELECT_FINISHED) {
            bool inside_start = ((point->outer > rend->selection.start.outer - 1) ||
                                 (point->outer == rend->selection.start.outer - 1 &&
                                  point->inner >= rend->selection.start.inner));
            bool inside_end = ((point->outer < rend->selection.end.outer - 1) ||
                               (point->outer == rend->selection.end.outer - 1 &&
                                point->inner <= rend->selection.end.inner));
            if (inside_start && inside_end) {
                foreground = cfg.selection_fg_color;
                background = rend->selection.bg_color;
            }
        }
    }

    SDL_Rect rect = {point->x * rend->font_width, point->y * rend->font_height, 0, 0};
    uint64_t old_y = point->y;
    int width = coord_trans(point, rend->window_cols, seq[0]);
    point->inner += strlen(seq) - 1;

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
    if (seq[0] == '\t') {
        rect.w = width * rend->font_width;
        rect.h = rend->font_height;
        SDL_FillRect(window_surface, &rect, background);
    } else {
        const char binseq[5] = {1};
        const char* seq2 = (seq[0] != '\0' ? seq : binseq);
        SDL_Surface* s = rasterize_code_point_cached(rend, seq2, foreground);
        rect.w = rend->font_width;
        rect.h = rend->font_height;
        SDL_FillRect(window_surface, &rect, background);
        SDL_BlitSurface(s, NULL, window_surface, &rect);
    }

    if (underline) {
        // TODO cache
        int baseline = TTF_FontAscent(rend->font) + 1;
        SDL_Rect ur = {};
        ur.x = rect.x;
        ur.y = rect.y + baseline;
        ur.w = rect.w;
        ur.h = 1;
        SDL_Color fgc = cfg.theme[foreground];
        uint32_t fg32 = SDL_MapRGB(window_surface->format, fgc.r, fgc.g, fgc.b);
        SDL_FillRect(window_surface, &ur, fg32);
    }

    return true;
}

size_t find_visbacklog(Render_State* rend, uint64_t the_id) {
    for (size_t i = 0; i < rend->visbacklogs.len; ++i) {
        Backlog_State* backlog = rend->visbacklogs[i];
        if (backlog->id == the_id) {
            return i;
        }
    }
    return -1;
}
