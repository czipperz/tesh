#include <SDL.h>
#include <SDL_ttf.h>
#include <stdint.h>
#include <stdio.h>
#include <Tracy.hpp>
#include <cz/defer.hpp>
#include <cz/env.hpp>
#include <cz/heap.hpp>
#include <cz/process.hpp>
#include <cz/string.hpp>
#include <cz/util.hpp>
#include <cz/vector.hpp>
#include <cz/working_directory.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <shellscalingapi.h>
#include <windows.h>
#endif

#include "global.hpp"
#include "shell.hpp"

///////////////////////////////////////////////////////////////////////////////
// Type definitions
///////////////////////////////////////////////////////////////////////////////

struct Visual_Point {
    int y;            // visual y
    int x;            // visual x
    uint64_t index;   // absolute position
    uint64_t line;    // line number
    uint64_t column;  // column number
};

struct Render_State {
    TTF_Font* font;
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

    bool hands_off;  // True if no user actions have occurred since the last command was ran.

    SDL_Color prompt_fg_color;
};

enum Backlog_Event_Type {
    BACKLOG_EVENT_START_PROCESS,
    BACKLOG_EVENT_START_INPUT,
    BACKLOG_EVENT_START_PROMPT,
};

struct Backlog_Event {
    uint64_t index;
    uint8_t type;
    union {
        uint64_t process_id;
    } v;
};

struct Backlog_State {
#define BUFFER_SIZE 4096
#define OUTER_INDEX(index) ((index) >> 12)
#define INNER_INDEX(index) ((index)&0xfff)
    cz::Vector<char*> buffers;
    uint64_t length;
    cz::Vector<Backlog_Event> events;
    uint64_t last_process_id;
};

struct Prompt_State {
    cz::Str prefix;
    cz::String text;
    size_t cursor;
    uint64_t process_id;
    uint64_t history_counter;
    cz::Vector<cz::Str> history;
    cz::Buffer_Array history_arena;
};

///////////////////////////////////////////////////////////////////////////////
// Configuration
///////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32
const char* font_path = "C:/Windows/Fonts/MesloLGM-Regular.ttf";
#else
const char* font_path = "/usr/share/fonts/TTF/MesloLGMDZ-Regular.ttf";
#endif
int font_size = 12;
int tab_width = 8;

SDL_Color process_colors[] = {
    {0x18, 0, 0, 0xff},    {0, 0x18, 0, 0xff},    {0, 0, 0x26, 0xff},
    {0x11, 0x11, 0, 0xff}, {0, 0x11, 0x11, 0xff}, {0x11, 0, 0x17, 0xff},
};

///////////////////////////////////////////////////////////////////////////////
// Renderer methods
///////////////////////////////////////////////////////////////////////////////

static void close_font(Render_State* rend) {
    ZoneScoped;
    for (int i = 0; i < CZ_DIM(rend->backlog_cache); i++) {
        SDL_FreeSurface(rend->backlog_cache[i]);
        rend->backlog_cache[i] = NULL;
    }
    for (int i = 0; i < CZ_DIM(rend->prompt_cache); i++) {
        SDL_FreeSurface(rend->prompt_cache[i]);
        rend->prompt_cache[i] = NULL;
    }
    TTF_CloseFont(rend->font);
}

static TTF_Font* open_font(const char* path, int font_size) {
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

    char text[2] = {ch, 0};
    SDL_Surface* surface = rasterize_character(text, rend->font, 0, color);
    CZ_ASSERT(surface);
    cache[index] = surface;
    return surface;
}

static int coord_trans(Visual_Point* point, int num_cols, char ch) {
    ++point->index;

    if (ch == '\n') {
        ++point->y;
        point->x = 0;
        ++point->line;
        point->column = 0;
        return 0;
    }

    int width = 1;
    if (ch == '\t') {
        uint64_t lcol2 = point->column;
        lcol2 += tab_width;
        lcol2 -= lcol2 % tab_width;
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

static void render_char(SDL_Surface* window_surface,
                        Render_State* rend,
                        Visual_Point* point,
                        SDL_Surface** cache,
                        uint32_t background,
                        SDL_Color foreground,
                        char c) {
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
            return;

        // Newlines aren't drawn.
        if (width == 0)
            return;
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
}

static void render_backlog(SDL_Surface* window_surface,
                           Render_State* rend,
                           Backlog_State* backlog) {
    ZoneScoped;
    Visual_Point* point = &rend->backlog_end;
    uint64_t i = rend->backlog_end.index;

    if (point->y >= rend->window_rows_ru)
        return;

    uint64_t process_id = 0;
    SDL_Color bg_color = process_colors[process_id % CZ_DIM(process_colors)];
    uint32_t background = SDL_MapRGB(window_surface->format, bg_color.r, bg_color.g, bg_color.b);

    SDL_Surface** cache = rend->backlog_cache;
    SDL_Color fg_color = rend->backlog_fg_color;

    size_t event_index = 0;

    for (; i < backlog->length; ++i) {
        while (event_index < backlog->events.len && backlog->events[event_index].index <= i) {
            Backlog_Event* event = &backlog->events[event_index];
            if (event->type == BACKLOG_EVENT_START_PROCESS) {
                process_id = event->v.process_id;
                if (process_id == -1)
                    bg_color = {};
                else
                    bg_color = process_colors[process_id % CZ_DIM(process_colors)];
                background = SDL_MapRGB(window_surface->format, bg_color.r, bg_color.g, bg_color.b);
                cache = rend->backlog_cache;
                fg_color = rend->backlog_fg_color;
            } else if (event->type == BACKLOG_EVENT_START_INPUT) {
                cache = rend->prompt_cache;
                fg_color = rend->prompt_fg_color;
            } else if (event->type == BACKLOG_EVENT_START_PROMPT) {
                // Ignore
            } else {
                CZ_PANIC("unreachable");
            }
            ++event_index;
        }

        char c = backlog->buffers[OUTER_INDEX(i)][INNER_INDEX(i)];
        render_char(window_surface, rend, point, cache, background, fg_color, c);
    }

    rend->backlog_end.index = i;
}

static void render_prompt(SDL_Surface* window_surface,
                          Render_State* rend,
                          Prompt_State* prompt,
                          Shell_State* shell) {
    ZoneScoped;

    Visual_Point point = rend->backlog_end;
    if (point.x != 0 || point.y != 0) {
        SDL_Rect rect = {point.x * rend->font_width, point.y * rend->font_height,
                         window_surface->w - point.x, rend->font_height};
        SDL_FillRect(window_surface, &rect, SDL_MapRGB(window_surface->format, 0, 0, 0));
        point.x = 0;
        ++point.y;
    }

    SDL_Color bg_color = process_colors[prompt->process_id % CZ_DIM(process_colors)];
    uint32_t background = SDL_MapRGB(window_surface->format, bg_color.r, bg_color.g, bg_color.b);

    for (size_t i = 0; i < shell->working_directory.len; ++i) {
        char c = shell->working_directory[i];
        render_char(window_surface, rend, &point, rend->backlog_cache, background,
                    rend->backlog_fg_color, c);
    }

    render_char(window_surface, rend, &point, rend->backlog_cache, background,
                rend->backlog_fg_color, ' ');

    for (size_t i = 0; i < prompt->prefix.len; ++i) {
        char c = prompt->prefix[i];
        render_char(window_surface, rend, &point, rend->backlog_cache, background,
                    rend->backlog_fg_color, c);
    }

    for (size_t i = 0; i < prompt->text.len; ++i) {
        char c = prompt->text[i];

        if (prompt->cursor == i) {
            SDL_Rect fill_rect = {point.x * rend->font_width - 1, point.y * rend->font_height, 2,
                                  rend->font_height};
            uint32_t foreground = SDL_MapRGB(window_surface->format, rend->prompt_fg_color.r,
                                             rend->prompt_fg_color.g, rend->prompt_fg_color.b);
            SDL_FillRect(window_surface, &fill_rect, foreground);

            render_char(window_surface, rend, &point, rend->prompt_cache, background,
                        rend->prompt_fg_color, c);

            if (point.x != 0) {
                SDL_FillRect(window_surface, &fill_rect, foreground);
            }
        } else {
            render_char(window_surface, rend, &point, rend->prompt_cache, background,
                        rend->prompt_fg_color, c);
        }
    }

    // Fill rest of line.
    {
        SDL_Rect fill_rect = {point.x * rend->font_width, point.y * rend->font_height, 0,
                              rend->font_height};
        fill_rect.w = window_surface->w - fill_rect.x;
        SDL_FillRect(window_surface, &fill_rect, background);
    }

    if (prompt->cursor == prompt->text.len) {
        SDL_Rect fill_rect = {point.x * rend->font_width - 1, point.y * rend->font_height, 2,
                              rend->font_height};
        uint32_t foreground = SDL_MapRGB(window_surface->format, rend->prompt_fg_color.r,
                                         rend->prompt_fg_color.g, rend->prompt_fg_color.b);
        SDL_FillRect(window_surface, &fill_rect, foreground);
    }
}

static void ensure_prompt_on_screen(Render_State* rend, Backlog_State* backlog);
static void auto_scroll_start_paging(Render_State* rend, Backlog_State* backlog) {
    uint64_t prompt_position = 0;
    for (size_t e = backlog->events.len; e-- > 0;) {
        Backlog_Event* event = &backlog->events[e];
        if (event->type == BACKLOG_EVENT_START_PROMPT) {
            prompt_position = event->index;
            break;
        }
    }

    // If we put the previous prompt at the top what happens.
    Visual_Point point = {};
    point.index = prompt_position;
    while (point.index < backlog->length && point.y + 3 < rend->window_rows) {
        char c = backlog->buffers[OUTER_INDEX(point.index)][INNER_INDEX(point.index)];
        coord_trans(&point, rend->window_cols, c);
    }

    if (point.y + 3 >= rend->window_rows) {
        rend->backlog_start = {};
        rend->backlog_start.index = prompt_position;
        rend->complete_redraw = true;
        // Reach maximum scroll so stop.
        rend->hands_off = false;
    } else {
        ensure_prompt_on_screen(rend, backlog);
    }
}

static void render_frame(SDL_Window* window,
                         Render_State* rend,
                         Backlog_State* backlog,
                         Prompt_State* prompt,
                         Shell_State* shell) {
    ZoneScoped;

    SDL_Surface* window_surface = SDL_GetWindowSurface(window);
    rend->window_rows = window_surface->h / rend->font_height;
    rend->window_rows_ru = (window_surface->h + rend->font_height - 1) / rend->font_height;
    rend->window_cols = window_surface->w / rend->font_width;

    if (rend->hands_off)
        auto_scroll_start_paging(rend, backlog);

    if (rend->complete_redraw) {
        ZoneScopedN("draw_background");
        SDL_FillRect(window_surface, NULL, SDL_MapRGB(window_surface->format, 0x00, 0x00, 0x00));
        rend->backlog_end = rend->backlog_start;
    }

    render_backlog(window_surface, rend, backlog);
    render_prompt(window_surface, rend, prompt, shell);

    {
        const SDL_Rect rects[] = {{0, 0, window_surface->w, window_surface->h}};
        ZoneScopedN("update_window_surface");
        SDL_UpdateWindowSurfaceRects(window, rects, CZ_DIM(rects));
    }

    rend->complete_redraw = false;
}

///////////////////////////////////////////////////////////////////////////////
// Buffer methods
///////////////////////////////////////////////////////////////////////////////

static void set_backlog_process(Backlog_State* backlog, uint64_t process_id) {
    Backlog_Event event = {};
    event.index = backlog->length;
    if (process_id == -2) {
        event.type = BACKLOG_EVENT_START_INPUT;
    } else if (process_id == -3) {
        event.type = BACKLOG_EVENT_START_PROMPT;
    } else {
        event.type = BACKLOG_EVENT_START_PROCESS;
        event.v.process_id = process_id;
    }
    backlog->events.reserve(cz::heap_allocator(), 1);
    backlog->events.push(event);
    backlog->last_process_id = process_id;
}

static void append_text(Backlog_State* backlog, uint64_t process_id, cz::Str text) {
    if (process_id != backlog->last_process_id)
        set_backlog_process(backlog, process_id);

    uint64_t overhang = INNER_INDEX(backlog->length + text.len);
    uint64_t inner = INNER_INDEX(backlog->length);
    if (overhang < text.len) {
        uint64_t underhang = text.len - overhang;
        if (underhang > 0) {
            memcpy(backlog->buffers.last() + inner, text.buffer + 0, underhang);
        }

        backlog->buffers.reserve(cz::heap_allocator(), 1);
        char* buffer = (char*)cz::heap_allocator().alloc({4096, 1});
        CZ_ASSERT(buffer);
        backlog->buffers.push(buffer);

        memcpy(backlog->buffers.last() + 0, text.buffer + underhang, overhang);
    } else {
        memcpy(backlog->buffers.last() + inner, text.buffer, text.len);
    }
    backlog->length += text.len;
}

///////////////////////////////////////////////////////////////////////////////
// Process control
///////////////////////////////////////////////////////////////////////////////

static bool run_line(Shell_State* shell, cz::Str text, uint64_t id) {
    Error error;
    Parse_Line line = {};

    cz::Buffer_Array arena;
    if (shell->arenas.len > 0)
        arena = shell->arenas.pop();
    else
        arena.init();

    error = parse_line(shell, arena.allocator(), &line, text);
    if (error != Error_Success)
        return false;

    error = start_execute_line(shell, arena, line, id);
    if (error != Error_Success)
        return false;

    return true;
}

static bool read_process_data(Shell_State* shell, Backlog_State* backlog) {
    static char buffer[4096];
    bool changes = false;
    for (size_t i = 0; i < shell->lines.len; ++i) {
        Running_Line* process = &shell->lines[i];

        for (size_t p = 0; p < process->pipeline.len; ++p) {
            Running_Program* program = &process->pipeline[p];
            int exit_code = 1;
            if (tick_program(shell, program, &exit_code)) {
                process->pipeline.remove(p);
                --p;
            }
        }

        if (process->out.is_open()) {
            int64_t result = 0;
            while (1) {
                result = process->out.read_text(buffer, sizeof(buffer), &process->out_carry);
                if (result <= 0)
                    break;
                append_text(backlog, process->id, {buffer, (size_t)result});
                changes = true;
            }

            if (result == 0) {
                process->out.close();
                process->out = {};
            }
        }

        if (process->pipeline.len == 0) {
            process->in.close();
            process->out.close();

            cz::Buffer_Array arena = process->arena;
            arena.clear();
            shell->arenas.reserve(cz::heap_allocator(), 1);
            shell->arenas.push(arena);

            shell->lines.remove(i);
            --i;
        }
    }
    return changes;
}

///////////////////////////////////////////////////////////////////////////////
// User events
///////////////////////////////////////////////////////////////////////////////

static void scroll_down(Render_State* rend, Backlog_State* backlog, int lines) {
    Visual_Point* start = &rend->backlog_start;
    int desired_y = start->y + lines;
    while (start->y < desired_y && start->index < backlog->length) {
        char c = backlog->buffers[OUTER_INDEX(start->index)][INNER_INDEX(start->index)];
        coord_trans(start, rend->window_cols, c);
    }
    start->y = 0;
}

static void scroll_up(Render_State* rend, Backlog_State* backlog, int lines) {
    ++lines;

    Visual_Point* start = &rend->backlog_start;
    uint64_t cursor = start->index;
    // int desired_y = start->y - lines;

    cz::Vector<Visual_Point> visual_sols = {};
    visual_sols.reserve_exact(temp_allocator, lines);
    visual_sols.len = lines;

    // No good way to go backwards so we do it one physical line at a time.
    // TODO: most of the time we should be able to a much simpler loop
    while (1) {
        if (cursor == 0)
            return;

        // Find start of line.
        uint64_t sol = cursor - 1;
        while (sol > 0) {
            --sol;
            char c = backlog->buffers[OUTER_INDEX(sol)][INNER_INDEX(sol)];
            if (c == '\n') {
                ++sol;
                break;
            }
        }

        // Loop through each visual line in this line.
        size_t visual_sols_index = 0;
        size_t visual_sols_len = 0;
        Visual_Point point = {};
        point.index = sol;

        // Start of physical line is also the start of a visual line.
        visual_sols[visual_sols_index] = point;
        visual_sols_index = (visual_sols_index + 1 % lines);
        ++visual_sols_len;

        while (1) {
            if (point.index >= cursor) {
                // CZ_DEBUG_ASSERT(point.index == cursor);  // not sure if > case happens
                break;
            }

            int old_y = point.y;
            int width = 0;
            while (point.y == old_y) {
                char c = backlog->buffers[OUTER_INDEX(point.index)][INNER_INDEX(point.index)];
                width = coord_trans(&point, rend->window_cols, c);
            }

            Visual_Point point2 = point;
            point2.x -= width;
            point2.column -= width;
            if (width > 0)
                point2.index--;
            visual_sols[visual_sols_index] = point2;
            visual_sols_index = (visual_sols_index + 1 % lines);
            ++visual_sols_len;
        }

        CZ_DEBUG_ASSERT(visual_sols_len >= 2);
        --visual_sols_len;
        if (visual_sols_index == 0)
            visual_sols_index = lines;
        --visual_sols_index;

        size_t desired = 0;
        if (visual_sols_len >= lines)
            desired = visual_sols_index;

        *start = visual_sols[desired];
        start->y = 0;
        cursor = start->index;

        if (lines <= visual_sols_len)
            break;
        lines -= visual_sols_len;
    }

#if 0
    uint64_t* line_start = &rend->backlog_scroll_screen_start;
    uint64_t cursor = *line_start;
    while (lines > 0 && cursor > 0) {
        --cursor;
        char c = backlog->buffers[OUTER_INDEX(cursor)][INNER_INDEX(cursor)];
        if (c == '\n') {
            *line_start = cursor + 1;
            --lines;
        }
    }
#endif
}

static void ensure_prompt_on_screen(Render_State* rend, Backlog_State* backlog) {
    if (rend->window_rows > 3) {
        Visual_Point backup = rend->backlog_start;
        rend->backlog_start = {};
        rend->backlog_start.index = backlog->length;
        scroll_up(rend, backlog, rend->window_rows - 3);
        if (rend->backlog_start.index > backup.index)
            rend->complete_redraw = true;
        else
            rend->backlog_start = backup;
    }
}

static void transform_shift_numbers(SDL_Keysym* keysym) {
    if (!(keysym->mod & KMOD_SHIFT))
        return;

    switch (keysym->sym) {
    case SDLK_1:
        keysym->sym = SDLK_EXCLAIM;
        break;
    case SDLK_2:
        keysym->sym = SDLK_AT;
        break;
    case SDLK_3:
        keysym->sym = SDLK_HASH;
        break;
    case SDLK_4:
        keysym->sym = SDLK_DOLLAR;
        break;
    case SDLK_5:
        keysym->sym = SDLK_PERCENT;
        break;
    case SDLK_6:
        keysym->sym = SDLK_CARET;
        break;
    case SDLK_7:
        keysym->sym = SDLK_AMPERSAND;
        break;
    case SDLK_8:
        keysym->sym = SDLK_ASTERISK;
        break;
    case SDLK_9:
        keysym->sym = SDLK_LEFTPAREN;
        break;
    case SDLK_0:
        keysym->sym = SDLK_RIGHTPAREN;
        break;
    case SDLK_SEMICOLON:
        keysym->sym = SDLK_COLON;
        break;
    case SDLK_COMMA:
        keysym->sym = SDLK_LESS;
        break;
    case SDLK_PERIOD:
        keysym->sym = SDLK_GREATER;
        break;
    case SDLK_MINUS:
        keysym->sym = SDLK_UNDERSCORE;
        break;
    case SDLK_EQUALS:
        keysym->sym = SDLK_PLUS;
        break;
    case SDLK_SLASH:
        keysym->sym = SDLK_QUESTION;
        break;
    case SDLK_QUOTE:
        keysym->sym = SDLK_QUOTEDBL;
        break;
    default:
        return;
    }
    keysym->mod &= ~KMOD_SHIFT;
}

static int process_events(Backlog_State* backlog,
                          Prompt_State* prompt,
                          Render_State* rend,
                          Shell_State* shell) {
    int num_events = 0;
    for (SDL_Event event; SDL_PollEvent(&event);) {
        ZoneScopedN("process_event");
        rend->hands_off = false;
        switch (event.type) {
        case SDL_QUIT:
            return -1;

        case SDL_WINDOWEVENT:
            // Ignore mouse window events.
            if (event.window.event == SDL_WINDOWEVENT_ENTER ||
                event.window.event == SDL_WINDOWEVENT_LEAVE ||
                event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ||
                event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                continue;
            }

            rend->complete_redraw = true;
            ++num_events;
            break;

        case SDL_KEYDOWN: {
            transform_shift_numbers(&event.key.keysym);

            int mod = (event.key.keysym.mod & ~(KMOD_CAPS | KMOD_NUM));
            if (mod & KMOD_ALT)
                mod |= KMOD_ALT;
            if (mod & KMOD_CTRL)
                mod |= KMOD_CTRL;
            if (mod & KMOD_SHIFT)
                mod |= KMOD_SHIFT;

            // Ignore the GUI key.
            mod &= ~KMOD_GUI;

            if (event.key.keysym.sym == SDLK_ESCAPE)
                return -1;
            if ((mod == KMOD_CTRL && event.key.keysym.sym == SDLK_c) ||
                event.key.keysym.sym == SDLK_RETURN) {
                append_text(backlog, -1, "\n");
                if (rend->backlog_start.index + 1 == backlog->length) {
                    ++rend->backlog_start.index;
                    rend->backlog_end = rend->backlog_start;
                }
                set_backlog_process(backlog, -3);
                append_text(backlog, prompt->process_id, shell->working_directory);
                append_text(backlog, prompt->process_id, " ");
                append_text(backlog, prompt->process_id, prompt->prefix);
                append_text(backlog, -2, prompt->text);
                append_text(backlog, prompt->process_id, "\n");

                if (event.key.keysym.sym == SDLK_RETURN) {
                    rend->hands_off = true;
                    if (!run_line(shell, prompt->text, prompt->process_id)) {
                        append_text(backlog, prompt->process_id, "Error: failed to execute\n");
                    }
                } else {
                    // TODO: kill active process
                }

                prompt->history.reserve(cz::heap_allocator(), 1);
                prompt->history.push(prompt->text.clone(prompt->history_arena.allocator()));
                prompt->history_counter = prompt->history.len;

                prompt->text.len = 0;
                prompt->cursor = 0;
                ++prompt->process_id;

                ensure_prompt_on_screen(rend, backlog);
                ++num_events;
            }
            if (mod == 0 && event.key.keysym.sym == SDLK_BACKSPACE) {
                if (prompt->cursor > 0) {
                    --prompt->cursor;
                    prompt->text.remove(prompt->cursor);
                }
                ensure_prompt_on_screen(rend, backlog);
                ++num_events;
            }
            if (mod == (KMOD_CTRL | KMOD_ALT) && event.key.keysym.sym == SDLK_BACKSPACE) {
                prompt->text.remove_range(0, prompt->cursor);
                prompt->cursor = 0;
                ensure_prompt_on_screen(rend, backlog);
                ++num_events;
            }
            if ((mod == 0 && event.key.keysym.sym == SDLK_LEFT) ||
                (mod == KMOD_CTRL && event.key.keysym.sym == SDLK_b)) {
                if (prompt->cursor > 0) {
                    --prompt->cursor;
                }
                ensure_prompt_on_screen(rend, backlog);
                ++num_events;
            }
            if ((mod == 0 && event.key.keysym.sym == SDLK_RIGHT) ||
                (mod == KMOD_CTRL && event.key.keysym.sym == SDLK_f)) {
                if (prompt->cursor < prompt->text.len) {
                    ++prompt->cursor;
                }
                ensure_prompt_on_screen(rend, backlog);
                ++num_events;
            }
            if ((mod == 0 && event.key.keysym.sym == SDLK_UP) ||
                (mod == KMOD_ALT && event.key.keysym.sym == SDLK_p)) {
                if (prompt->history_counter > 0) {
                    --prompt->history_counter;
                    prompt->text.len = 0;
                    cz::Str hist = prompt->history[prompt->history_counter];
                    prompt->text.reserve(cz::heap_allocator(), hist.len);
                    prompt->text.append(hist);
                    prompt->cursor = prompt->text.len;
                }
                ensure_prompt_on_screen(rend, backlog);
                ++num_events;
            }
            if ((mod == 0 && event.key.keysym.sym == SDLK_DOWN) ||
                (mod == KMOD_ALT && event.key.keysym.sym == SDLK_n)) {
                if (prompt->history_counter < prompt->history.len) {
                    ++prompt->history_counter;
                    prompt->text.len = 0;
                    if (prompt->history_counter < prompt->history.len) {
                        cz::Str hist = prompt->history[prompt->history_counter];
                        prompt->text.reserve(cz::heap_allocator(), hist.len);
                        prompt->text.append(hist);
                    }
                    prompt->cursor = prompt->text.len;
                }
                ensure_prompt_on_screen(rend, backlog);
                ++num_events;
            }
            if ((mod == 0 && event.key.keysym.sym == SDLK_HOME) ||
                (mod == KMOD_ALT && event.key.keysym.sym == SDLK_a)) {
                prompt->cursor = 0;
                ensure_prompt_on_screen(rend, backlog);
                ++num_events;
            }
            if ((mod == 0 && event.key.keysym.sym == SDLK_END) ||
                (mod == KMOD_ALT && event.key.keysym.sym == SDLK_e)) {
                prompt->cursor = prompt->text.len;
                ensure_prompt_on_screen(rend, backlog);
                ++num_events;
            }
            if (mod == KMOD_CTRL && event.key.keysym.sym == SDLK_l) {
                rend->backlog_start = {};
                rend->backlog_start.index = backlog->length;
                rend->complete_redraw = true;
                ++num_events;
            }
            if (mod == KMOD_CTRL && event.key.keysym.sym == SDLK_v) {
                int lines = cz::max(rend->window_rows, 6) - 3;
                scroll_down(rend, backlog, lines);
                rend->complete_redraw = true;
                ++num_events;
            }
            if (mod == KMOD_ALT && event.key.keysym.sym == SDLK_v) {
                int lines = cz::max(rend->window_rows, 6) - 3;
                scroll_up(rend, backlog, lines);
                rend->complete_redraw = true;
                ++num_events;
            }
            if (mod == KMOD_ALT && event.key.keysym.sym == SDLK_GREATER) {
                rend->backlog_start = {};
                rend->backlog_start.index = backlog->length;
                int lines = cz::max(rend->window_rows, 3) - 3;
                scroll_up(rend, backlog, lines);
                rend->complete_redraw = true;
                ++num_events;
            }
            if (mod == (KMOD_CTRL | KMOD_ALT) && event.key.keysym.sym == SDLK_b) {
                size_t event_index = 0;
                while (event_index < backlog->events.len &&
                       backlog->events[event_index].index < rend->backlog_start.index) {
                    ++event_index;
                }
                while (event_index-- > 0) {
                    Backlog_Event* event = &backlog->events[event_index];
                    if (event->type == BACKLOG_EVENT_START_PROMPT) {
                        rend->backlog_start = {};
                        rend->backlog_start.index = backlog->events[event_index].index;
                        rend->complete_redraw = true;
                        ++num_events;
                        break;
                    }
                }
            }
            if (mod == (KMOD_CTRL | KMOD_ALT) && event.key.keysym.sym == SDLK_f) {
                size_t event_index = 0;
                while (event_index < backlog->events.len &&
                       backlog->events[event_index].index <= rend->backlog_start.index) {
                    ++event_index;
                }
                for (; event_index < backlog->events.len; ++event_index) {
                    Backlog_Event* event = &backlog->events[event_index];
                    if (event->type == BACKLOG_EVENT_START_PROMPT) {
                        rend->backlog_start = {};
                        rend->backlog_start.index = backlog->events[event_index].index;
                        break;
                    }
                }
                if (event_index >= backlog->events.len) {
                    rend->backlog_start = {};
                    rend->backlog_start.index = backlog->length;
                }
                rend->complete_redraw = true;
                ++num_events;
            }
            if (mod == KMOD_SHIFT && event.key.keysym.sym == SDLK_INSERT) {
                char* clip = SDL_GetClipboardText();
                if (clip) {
                    CZ_DEFER(SDL_free(clip));
                    size_t len = strlen(clip);
                    cz::String str = {clip, len, len};
                    cz::strip_carriage_returns(&str);
                    prompt->text.reserve(cz::heap_allocator(), str.len);
                    prompt->text.insert(prompt->cursor, str);
                    prompt->cursor += str.len;
                    ++num_events;
                }
            }

            if (mod == KMOD_CTRL &&
                (event.key.keysym.sym == SDLK_PLUS || event.key.keysym.sym == SDLK_MINUS)) {
                int new_font_size = font_size;
                if (event.key.keysym.sym == SDLK_PLUS) {
                    new_font_size += 4;
                } else {
                    new_font_size = cz::max(new_font_size - 4, 4);
                }

                TTF_Font* new_font = open_font(font_path, (int)(new_font_size * rend->dpi_scale));
                if (new_font) {
                    close_font(rend);

                    rend->font = new_font;
                    font_size = new_font_size;
                    rend->font_height = TTF_FontLineSkip(rend->font);
                    // TODO: handle failure
                    TTF_GlyphMetrics(rend->font, ' ', nullptr, nullptr, nullptr, nullptr,
                                     &rend->font_width);

                    rend->complete_redraw = true;
                    ++num_events;
                }
            }
        } break;

        case SDL_TEXTINPUT: {
            const Uint8* state = SDL_GetKeyboardState(NULL);
            if (state[SDL_SCANCODE_LCTRL] || state[SDL_SCANCODE_RCTRL])
                break;
            cz::Str text = event.text.text;
            prompt->text.reserve(cz::heap_allocator(), text.len);
            prompt->text.insert(prompt->cursor, text);
            prompt->cursor += text.len;
            ensure_prompt_on_screen(rend, backlog);
            ++num_events;
        } break;

        case SDL_MOUSEWHEEL: {
            if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                event.wheel.y *= -1;
                event.wheel.x *= -1;
            }

// On linux the horizontal scroll is flipped for some reason.
#ifndef _WIN32
            event.wheel.x *= -1;
#endif

            event.wheel.y *= 4;
            event.wheel.x *= 10;

            if (event.wheel.y < 0) {
                scroll_down(rend, backlog, -event.wheel.y);
            } else if (event.wheel.y > 0) {
                scroll_up(rend, backlog, event.wheel.y);
            }

            rend->complete_redraw = true;
            ++num_events;
        } break;
        }
    }
    return num_events;
}

///////////////////////////////////////////////////////////////////////////////
// main
///////////////////////////////////////////////////////////////////////////////

int actual_main(int argc, char** argv) {
    Render_State rend = {};
    Backlog_State backlog = {};
    Prompt_State prompt = {};
    Shell_State shell = {};

    prompt.history_arena.init();

    cz::Buffer_Array temp_arena;
    temp_arena.init();
    temp_allocator = temp_arena.allocator();

    prompt.prefix = "$ ";
    rend.complete_redraw = true;

    if (!cz::get_working_directory(cz::heap_allocator(), &shell.working_directory)) {
        fprintf(stderr, "Failed to get working directory\n");
        return 1;
    }

    {
        cz::String home = {};
        if (cz::env::get_home(cz::heap_allocator(), &home)) {
            set_env_var(&shell, "HOME", home);
        }
    }

    {
        backlog.buffers.reserve(cz::heap_allocator(), 1);
        char* buffer = (char*)cz::heap_allocator().alloc({4096, 1});
        CZ_ASSERT(buffer);
        backlog.buffers.push(buffer);
    }

#ifdef _WIN32
    SetProcessDpiAwareness(PROCESS_SYSTEM_DPI_AWARE);
#endif

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    CZ_DEFER(SDL_Quit());

    if (TTF_Init() < 0) {
        fprintf(stderr, "TTF_Init failed: %s\n", TTF_GetError());
        return 1;
    }
    CZ_DEFER(TTF_Quit());

    rend.dpi_scale = 1.0f;
    {
        const float dpi_default = 96.0f;
        float dpi = 0;
        if (SDL_GetDisplayDPI(0, &dpi, NULL, NULL) == 0)
            rend.dpi_scale = dpi / dpi_default;
    }

    SDL_Window* window = SDL_CreateWindow(
        "tesh", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 800 * rend.dpi_scale,
        800 * rend.dpi_scale, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    CZ_DEFER(SDL_DestroyWindow(window));

    rend.font = open_font(font_path, (int)(font_size * rend.dpi_scale));
    if (!rend.font) {
        fprintf(stderr, "TTF_OpenFont failed: %s\n", SDL_GetError());
        return 1;
    }
    CZ_DEFER(TTF_CloseFont(rend.font));

    rend.font_height = TTF_FontLineSkip(rend.font);
    rend.font_width = 10;
    TTF_GlyphMetrics(rend.font, ' ', nullptr, nullptr, nullptr, nullptr, &rend.font_width);

    rend.backlog_fg_color = {0xdd, 0xdd, 0xdd, 0xff};
    rend.prompt_fg_color = {0x77, 0xf9, 0xff, 0xff};

    cz::env::set("PAGER", "cat");

    while (1) {
        uint32_t start_frame = SDL_GetTicks();

        temp_arena.clear();

        int status = process_events(&backlog, &prompt, &rend, &shell);
        if (status < 0)
            break;

        if (read_process_data(&shell, &backlog))
            status = 1;

        if (status > 0)
            render_frame(window, &rend, &backlog, &prompt, &shell);

        const uint32_t frame_length = 1000 / 60;
        uint32_t wanted_end = start_frame + frame_length;
        uint32_t end_frame = SDL_GetTicks();
        if (wanted_end > end_frame) {
            SDL_Delay(wanted_end - end_frame);
        }

        FrameMark;
    }

    return 0;
}
