#include "render.hpp"

#include <SDL_image.h>
#include <inttypes.h>
#include <cz/binary_search.hpp>
#include <cz/date.hpp>
#include <cz/format.hpp>
#include <cz/string.hpp>
#include <tracy/Tracy.hpp>

#include "backlog.hpp"
#include "config.hpp"
#include "global.hpp"
#include "prompt.hpp"
#include "search.hpp"
#include "shell.hpp"
#include "unicode.hpp"

#ifdef _WIN32
#include <SDL_syswm.h>
#endif

#include "UbuntuMono.h"

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

////////////////////////////////////////////////////////////////////////////////
// Font methods
////////////////////////////////////////////////////////////////////////////////

void init_font(Font_State* font, double dpi_scale) {
    font->size = cfg.default_font_size;
    resize_font(font->size, dpi_scale, font);

    // Old versions of SDL_ttf don't parse FontLineSkip correctly so we manually set it.
    font->height = cz::max(TTF_FontLineSkip(font->sdl), (int)(TTF_FontHeight(font->sdl) * 1.05f));
    font->width = 10;
    TTF_GlyphMetrics(font->sdl, ' ', nullptr, nullptr, nullptr, nullptr, &font->width);
}

void close_font(Font_State* font) {
    ZoneScoped;
    for (int ch = 0; ch < CZ_DIM(font->caches); ch++) {
        Surface_Cache* cache = &font->caches[ch];
        for (int i = 0; i < cache->surfaces.len; i++) {
            SDL_Surface** surface = &cache->surfaces.get(i);
            SDL_FreeSurface(*surface);
            *surface = NULL;
        }
        cache->code_points.len = 0;
        cache->surfaces.len = 0;
    }
    TTF_CloseFont(font->sdl);
}

void resize_font(int font_size, double dpi_scale, Font_State* font) {
    ZoneScoped;
    TTF_Font* new_font = NULL;
    int ptsize = (int)(font_size * dpi_scale);
    if (cfg.font_path.len > 0) {
        new_font = TTF_OpenFont(cfg.font_path.buffer, ptsize);
    }
    if (new_font == NULL) {
        // Load the default font instead.
        SDL_RWops* font_mem = SDL_RWFromConstMem(&UbuntuMonoData[0], sizeof(UbuntuMonoData));
        new_font = TTF_OpenFontRW(font_mem, /*freesrc=*/true, ptsize);
    }
    if (new_font) {
        close_font(font);

        font->sdl = new_font;
        font->size = font_size;
        // Old versions of SDL_ttf don't parse FontLineSkip correctly so we manually set it.
        font->height =
            cz::max(TTF_FontLineSkip(font->sdl), (int)(TTF_FontHeight(font->sdl) * 1.05f));
        // TODO: handle failure
        TTF_GlyphMetrics(font->sdl, ' ', nullptr, nullptr, nullptr, nullptr, &font->width);
    }
}

////////////////////////////////////////////////////////////////////////////////
// Drawing methods
////////////////////////////////////////////////////////////////////////////////

static SDL_Surface* rasterize_code_point(const char* text,
                                         TTF_Font* font,
                                         int style,
                                         SDL_Color fgc) {
    ZoneScoped;
    TTF_SetFontStyle(font, style);
    return TTF_RenderUTF8_Blended(font, text, fgc);
}

static SDL_Surface* rasterize_code_point_cached(Font_State* font,
                                                const char seq[5],
                                                uint8_t color256) {
    uint32_t code_point = unicode::utf8_code_point((const uint8_t*)seq);

    // Check the cache.
    Surface_Cache* cache = &font->caches[color256];
    size_t index;
    if (cz::binary_search(cache->code_points.as_slice(), code_point, &index))
        return cache->surfaces[index];  // Cache hit.

    // Cache miss.  Rasterize and add to the cache.
    SDL_Surface* surface = rasterize_code_point(seq, font->sdl, 0, cfg.theme[color256]);

    // I've seen this case actually come up before so re-render as an invalid character.
    if (!surface) {
        if (!strcmp(seq, "\1"))
            CZ_PANIC("Failed to render");
        return rasterize_code_point_cached(font, "\1", color256);
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
                       const SDL_Rect& grid_rect,
                       Render_State* rend,
                       Visual_Point* point,
                       uint32_t background,
                       uint8_t foreground,
                       bool underline,
                       const char seq[5],
                       bool set_tile) {
    if (set_tile) {
        size_t index = point->y * rend->grid_cols + point->x;
        if (index < rend->grid.len) {
            Visual_Tile* tile = &rend->grid[index];
            tile->outer = point->outer + 1;
            tile->inner = point->inner;

            if (seq[0] == '\n') {
                for (int x = point->x + 1; x < rend->grid_cols; ++x) {
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

    SDL_Rect rect = {grid_rect.x + point->x * rend->font.width,
                     grid_rect.y + point->y * rend->font.height, 0, 0};
    uint64_t old_y = point->y;
    int width = coord_trans(point, rend->grid_cols, seq[0]);
    point->inner += strlen(seq) - 1;

    if (point->y != old_y) {
        rect.w = grid_rect.w - (rect.x - grid_rect.x);
        rect.h = rend->font.height;
        SDL_FillRect(window_surface, &rect, background);

        rect.x = grid_rect.x;
        rect.y += rend->font.height;

        // Beyond bottom of screen.
        if (point->y >= rend->grid_rows_ru)
            return false;

        // Newlines aren't drawn.
        if (width == 0)
            return true;
    }

    ZoneScopedN("blit_character");
    if (seq[0] == '\t') {
        rect.w = width * rend->font.width;
        rect.h = rend->font.height;
        SDL_FillRect(window_surface, &rect, background);
    } else {
        const char binseq[5] = {1};
        const char* seq2 = (seq[0] != '\0' ? seq : binseq);
        SDL_Surface* s = rasterize_code_point_cached(&rend->font, seq2, foreground);
        rect.w = rend->font.width;
        rect.h = rend->font.height;
        SDL_FillRect(window_surface, &rect, background);
        SDL_BlitSurface(s, NULL, window_surface, &rect);
    }

    if (underline) {
        // TODO cache
        int baseline = TTF_FontAscent(rend->font.sdl) + 1;
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

float get_dpi_scale(SDL_Window* sdl_window) {
    int display = SDL_GetWindowDisplayIndex(sdl_window);
    if (display == -1)
        display = 0;

    const float dpi_default = 96.0f;
    float dpi = 0;
    if (SDL_GetDisplayDPI(display, &dpi, NULL, NULL) != 0)
        return 1.0f;  // failure so assume no scaling
    return dpi / dpi_default;
}

void load_cursors(Window_State* window) {
    window->default_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    // rend->select_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_IBEAM);
    window->click_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);

    CZ_ASSERT(window->default_cursor);
    // CZ_ASSERT(rend->select_cursor);
    CZ_ASSERT(window->click_cursor);
}

///////////////////////////////////////////////////////////////////////////////
// Stuff from main.cpp
///////////////////////////////////////////////////////////////////////////////

static void make_info(cz::String* info,
                      Render_State* rend,
                      Backlog_State* backlog,
                      uint64_t first_line_index,
                      std::chrono::steady_clock::time_point now) {
    if (backlog->cancelled)
        return;

    std::chrono::steady_clock::time_point end = backlog->end;
    if (!backlog->done)
        end = now;

    // Find the line number.
    size_t first_line_number = 0;
    if (cz::binary_search(backlog->lines.as_slice(), first_line_index, &first_line_number))
        ++first_line_number;  // Go after the match.
    // Find the max number of lines.  There's a free newline after the
    // prompt so we subtract 1 if there is no auto trailing newline.
    size_t max_lines = backlog->lines.len;
    if (backlog->length > 0 && backlog->get(backlog->length - 1) == '\n')
        max_lines--;
    cz::append(temp_allocator, info, 'L', first_line_number, '/', max_lines, ' ');

    uint64_t millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - backlog->start).count();

    if (backlog->done) {
        cz::append(temp_allocator, info, '(', backlog->exit_code, ") ");
    } else {
        uint64_t abs_millis = millis % 2000;
        if (abs_millis <= 666)
            cz::append(temp_allocator, info, ".   ");
        else if (abs_millis <= 1333)
            cz::append(temp_allocator, info, "..  ");
        else
            cz::append(temp_allocator, info, "... ");
    }

    if (cfg.backlog_info_render_date) {
        time_t tm = std::chrono::system_clock::to_time_t(backlog->start2);
        cz::Date date = cz::time_t_to_date_local(tm);
        cz::append_sprintf(temp_allocator, info, "%04d/%02d/%02d %02d:%02d:%02d", date.year,
                           date.month, date.day_of_month, date.hour, date.minute, date.second);
        cz::append(temp_allocator, info, ' ');
    }

    uint64_t seconds = millis / 1000;
    uint64_t minutes = seconds / 60;

    if (minutes > 0) {
        uint64_t hours = minutes / 60;
        if (hours > 0) {
            cz::append_sprintf(temp_allocator, info, "%" PRIu64, hours);
            cz::append_sprintf(temp_allocator, info, ":%.2u", (unsigned)(minutes % 60));
        } else {
            cz::append_sprintf(temp_allocator, info, "%u", (unsigned)(minutes % 60));
        }
        cz::append_sprintf(temp_allocator, info, ":%.2u", (unsigned)(seconds % 60));
    } else {
        cz::append_sprintf(temp_allocator, info, "%u", (unsigned)(seconds % 60));
    }
    cz::append_sprintf(temp_allocator, info, ".%.3us", (unsigned)(millis % 1000));
}

///////////////////////////////////////////////////////////////////////////////
// Draw strings
///////////////////////////////////////////////////////////////////////////////

static size_t make_string_code_point(char sequence[5], cz::Str info, size_t start) {
    size_t width = cz::min(unicode::utf8_width(sequence[0]), info.len - start);
    for (size_t off = 1; off < width; ++off) {
        char ch = info[start + off];
        if (!unicode::utf8_is_continuation(ch)) {
            // Invalid utf8 so treat the char as a single byte.
            width = 1;
            memset(sequence + 1, 0, 4);
            break;
        }
        sequence[off] = ch;
    }
    return width;
}

size_t make_backlog_code_point(char sequence[5], Backlog_State* backlog, size_t start) {
    size_t width = cz::min(unicode::utf8_width(sequence[0]), (size_t)backlog->length - start);
    for (size_t off = 1; off < width; ++off) {
        char ch = backlog->get(start + off);
        if (!unicode::utf8_is_continuation(ch)) {
            // Invalid utf8 so treat the char as a single byte.
            width = 1;
            memset(sequence + 1, 0, 4);
            break;
        }
        sequence[off] = ch;
    }
    return width;
}

static bool render_string(SDL_Surface* window_surface,
                          const SDL_Rect& grid_rect,
                          Render_State* rend,
                          Visual_Point* info_start,
                          uint32_t background,
                          uint8_t foreground,
                          cz::Str info,
                          bool set_tile) {
    for (size_t i = 0; i < info.len;) {
        // Get the chars that compose this code point.
        char seq[5] = {info[i]};
        i += make_string_code_point(seq, info, i);

        // Render this code point.
        if (!render_code_point(window_surface, grid_rect, rend, info_start, background, foreground,
                               false, seq, set_tile)) {
            return false;
        }
    }
    return true;
}

static void render_info(SDL_Surface* window_surface,
                        const SDL_Rect& grid_rect,
                        Render_State* rend,
                        Visual_Point info_start,
                        Visual_Point info_end,
                        uint32_t background,
                        cz::Str info,
                        Backlog_State* backlog) {
    if (rend->selection.type == SELECT_REGION || rend->selection.type == SELECT_FINISHED) {
        bool inside_start = ((info_end.outer > rend->selection.start.outer - 1) ||
                             (info_end.outer == rend->selection.start.outer - 1 &&
                              info_end.inner >= rend->selection.start.inner));
        bool inside_end = ((info_start.outer < rend->selection.end.outer - 1) ||
                           (info_start.outer == rend->selection.end.outer - 1 &&
                            info_start.inner <= rend->selection.end.inner));
        if (inside_start && inside_end)
            return;
    }

    uint8_t foreground = cfg.info_running_fg_color;
    if (backlog->done) {
        if (backlog->exit_code == 0)
            foreground = cfg.info_success_fg_color;
        else
            foreground = cfg.info_failure_fg_color;
    }
    render_string(window_surface, grid_rect, rend, &info_start, background, foreground, info,
                  false);
}

uint64_t render_length(Backlog_State* backlog) {
    if (backlog->render_collapsed && backlog->lines.len > 0)
        return backlog->lines[0];
    return backlog->length;
}

bool render_backlog(SDL_Surface* window_surface,
                    const SDL_Rect& grid_rect,
                    Render_State* rend,
                    Shell_State* shell,
                    Prompt_State* prompt,
                    cz::Slice<Backlog_State*> backlogs,
                    std::chrono::steady_clock::time_point now,
                    Backlog_State* backlog,
                    size_t visindex) {
    ZoneScoped;
    Visual_Point* point = &rend->backlog_end;
    uint64_t i = 0;
    if (point->outer == visindex) {
        i = point->inner;
    } else {
        point->outer++;
        point->inner = 0;
    }

    CZ_ASSERT(point->y >= 0);
    if (point->y >= rend->grid_rows_ru)
        return false;

    SDL_Color bg_color = cfg.process_colors[backlog->id % cfg.process_colors.len];
    if (rend->selected_outer == visindex) {
        bg_color.r *= 2;
        bg_color.g *= 2;
        bg_color.b *= 2;
    }
    uint32_t background = SDL_MapRGB(window_surface->format, bg_color.r, bg_color.g, bg_color.b);

    cz::String info = {};
    make_info(&info, rend, backlog, point->inner, now);
    bool info_has_start = false, info_has_end = false;
    Visual_Point info_start = {}, info_end = {};
    int info_y = point->y;
    int info_x_start = (int)(rend->grid_cols - info.len);

    uint8_t fg_color = cfg.backlog_fg_color;

    size_t event_index = 0;

    uint64_t end = render_length(backlog);
    bool inside_hyperlink = false;
    while (i < end) {
        while (event_index < backlog->events.len && backlog->events[event_index].index <= i) {
            Backlog_Event* event = &backlog->events[event_index];
            if (event->type == BACKLOG_EVENT_START_PROCESS) {
                fg_color = cfg.backlog_fg_color;
            } else if (event->type == BACKLOG_EVENT_START_INPUT) {
                fg_color = cfg.prompt_fg_color;
            } else if (event->type == BACKLOG_EVENT_START_DIRECTORY) {
                fg_color = cfg.directory_fg_color;
            } else if (event->type == BACKLOG_EVENT_SET_GRAPHIC_RENDITION) {
                uint64_t gr = event->payload;
                fg_color = (uint8_t)((gr & GR_FOREGROUND_MASK) >> GR_FOREGROUND_SHIFT);
            } else if (event->type == BACKLOG_EVENT_START_HYPERLINK) {
                inside_hyperlink = true;
            } else if (event->type == BACKLOG_EVENT_END_HYPERLINK) {
                inside_hyperlink = false;
            } else {
                CZ_PANIC("unreachable");
            }
            ++event_index;
        }

        Visual_Point old_point = *point;

        // Get the chars that compose this code point.
        char seq[5] = {backlog->get(i)};
        i += make_backlog_code_point(seq, backlog, i);

        bool underline = (SDL_GetModState() & KMOD_CTRL) != 0 && inside_hyperlink;
        if (!render_code_point(window_surface, grid_rect, rend, point, background, fg_color,
                               underline, seq, true)) {
            break;
        }

        if (!info_has_end && point->y != info_y) {
            info_has_end = true;
            info_end = old_point;
        }
        if (!info_has_start && (point->y != info_y || point->x > info_x_start - 1)) {
            info_has_start = true;
            info_start = old_point;
        }
    }

    if (rend->attached_outer == visindex) {
        render_prompt(window_surface, grid_rect, rend, prompt, /*search=*/nullptr, backlogs, shell);
    } else if (rend->backlog_end.inner == backlog->length && backlog->length > 0 &&
               backlog->get(backlog->length - 1) != '\n') {
        Visual_Point old_point = *point;

        if (!render_code_point(window_surface, grid_rect, rend, point, background,
                               cfg.prompt_fg_color, false, "\n", true)) {
            return false;
        }

        if (!info_has_end && point->y != info_y) {
            info_has_end = true;
            info_end = old_point;
        }
        if (!info_has_start && (point->y != info_y || point->x > info_x_start - 1)) {
            info_has_start = true;
            info_start = old_point;
        }
    }

    if (info_has_start && info.len < rend->grid_cols) {
        if (!info_has_end)
            info_end = info_start;
        info_start.x = info_x_start;
        render_info(window_surface, grid_rect, rend, info_start, info_end, background, info,
                    backlog);
    }

    bg_color = {};
    background = SDL_MapRGB(window_surface->format, bg_color.r, bg_color.g, bg_color.b);
    if (!render_code_point(window_surface, grid_rect, rend, point, background, cfg.prompt_fg_color,
                           false, "\n", true)) {
        return false;
    }

    return true;
}

void render_prompt(SDL_Surface* window_surface,
                   const SDL_Rect& grid_rect,
                   Render_State* rend,
                   Prompt_State* prompt,
                   Search_State* search,
                   cz::Slice<Backlog_State*> backlogs,
                   Shell_State* shell) {
    ZoneScoped;

    Visual_Point* point = &rend->backlog_end;
    Visual_Point temp_point = {};

    bool is_searching = (search && search->is_searching);
    if (is_searching) {
        prompt = &search->prompt;

        // Display prompt at bottom of screen no matter what.
        point = &temp_point;
        point->y = rend->grid_rows - 1;
    }

    if (rend->attached_outer == -1) {
        point->outer++;
        point->inner = 0;
    }

    uint64_t process_id =
        (rend->attached_outer == -1 ? backlogs.len : rend->visbacklogs[rend->attached_outer]->id);
    SDL_Color bg_color = cfg.process_colors[process_id % cfg.process_colors.len];
    if (rend->selected_outer == -1 || rend->attached_outer == rend->selected_outer) {
        bg_color.r *= 2;
        bg_color.g *= 2;
        bg_color.b *= 2;
    }
    uint32_t background = SDL_MapRGB(window_surface->format, bg_color.r, bg_color.g, bg_color.b);

    if (is_searching) {
        render_string(window_surface, grid_rect, rend, point, background, cfg.directory_fg_color,
                      prompt->prefix, true);
    } else if (rend->attached_outer == -1) {
        render_string(window_surface, grid_rect, rend, point, background, cfg.directory_fg_color,
                      get_wd(&shell->local), true);
        render_string(window_surface, grid_rect, rend, point, background, cfg.backlog_fg_color,
                      prompt->prefix, true);
    }

    bool drawn_cursor = false;
    SDL_Color prompt_fg_color = cfg.theme[cfg.prompt_fg_color];
    uint32_t cursor_color =
        SDL_MapRGB(window_surface->format, prompt_fg_color.r, prompt_fg_color.g, prompt_fg_color.b);
    for (size_t i = 0; i < prompt->text.len;) {
        bool draw_cursor = (!drawn_cursor && i >= prompt->cursor);
        SDL_Rect cursor_rect;
        if (draw_cursor) {
            cursor_rect = {grid_rect.x + point->x * rend->font.width - 1,
                           grid_rect.y + point->y * rend->font.height, 2, rend->font.height};
            SDL_FillRect(window_surface, &cursor_rect, cursor_color);
            drawn_cursor = true;
        }

        // Get the chars that compose this code point.
        char seq[5] = {prompt->text[i]};
        i += make_string_code_point(seq, prompt->text, i);

        // Render this code point.
        render_code_point(window_surface, grid_rect, rend, point, background, cfg.prompt_fg_color,
                          false, seq, true);

        // Draw cursor.
        if (draw_cursor && point->x != 0) {
            SDL_FillRect(window_surface, &cursor_rect, cursor_color);
        }
    }

    // Fill rest of line.
    Visual_Point eol = *point;
    render_code_point(window_surface, grid_rect, rend, point, background, cfg.backlog_fg_color,
                      false, "\n", true);

    if (prompt->cursor == prompt->text.len) {
        // Draw cursor.
        SDL_Rect cursor_rect = {grid_rect.x + eol.x * rend->font.width - 1,
                                grid_rect.y + eol.y * rend->font.height, 2, rend->font.height};
        SDL_FillRect(window_surface, &cursor_rect, cursor_color);
    }

    if (prompt->history_searching) {
        cz::Str prefix = "History:\n";
        render_string(window_surface, grid_rect, rend, point, background, cfg.backlog_fg_color,
                      prefix, true);

        cz::Vector<cz::Str>* history = prompt_history(prompt, rend->attached_outer != -1);
        for (size_t i = history->len; i-- > 0;) {
            cz::Str hist = (*history)[i];
            if (hist.contains_case_insensitive(prompt->text)) {
                uint8_t color = cfg.backlog_fg_color;
                if (prompt->history_counter == i) {
                    // TODO: we should probably have a custom bg color as
                    // well because spaces will be invisible otherwise.
                    color = cfg.selected_completion_fg_color;
                }
                if (!render_string(window_surface, grid_rect, rend, point, background, color, hist,
                                   true)) {
                    break;
                }
                if (!render_code_point(window_surface, grid_rect, rend, point, background,
                                       cfg.backlog_fg_color, false, "\n", true)) {
                    break;
                }
            }
        }

        render_code_point(window_surface, grid_rect, rend, point, background, cfg.backlog_fg_color,
                          false, "\n", true);
    }

    if (prompt->completion.is) {
        cz::Str prefix = "Completions:\n";
        render_string(window_surface, grid_rect, rend, point, background, cfg.backlog_fg_color,
                      prefix, true);
        size_t longest_entry = 0;
        for (int i = 0; i < prompt->completion.results.len; i++) {
            longest_entry = cz::max(longest_entry, prompt->completion.results[i].len);
        }

        size_t chars_on_line = 0;
        for (int i = 0; i < prompt->completion.results.len; i++) {
            cz::Str result = prompt->completion.results[i];
            uint8_t color = cfg.backlog_fg_color;
            if (prompt->completion.current == i) {
                // TODO: we should probably have a custom bg color as
                // well because spaces will be invisible otherwise.
                color = cfg.selected_completion_fg_color;
            }
            render_string(window_surface, grid_rect, rend, point, background, color, result, true);

            for (size_t padding = result.len; padding < longest_entry + 1; padding++) {
                render_code_point(window_surface, grid_rect, rend, point, background,
                                  cfg.backlog_fg_color, false, " ", true);
            }

            chars_on_line += longest_entry + 1;
            if (chars_on_line + longest_entry + 1 > rend->grid_cols) {
                render_code_point(window_surface, grid_rect, rend, point, background,
                                  cfg.backlog_fg_color, false, "\n", true);
                chars_on_line = 0;
            }
        }
        if (chars_on_line != 0) {
            render_code_point(window_surface, grid_rect, rend, point, background,
                              cfg.backlog_fg_color, false, "\n", true);
        }
    }
}
