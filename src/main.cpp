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
#include <shellscalingapi.h>
#include <windows.h>
#endif

#include "global.hpp"
#include "shell.hpp"

///////////////////////////////////////////////////////////////////////////////
// Type definitions
///////////////////////////////////////////////////////////////////////////////

struct Render_State {
    TTF_Font* font;
    int font_width;
    int font_height;
    int window_height;
    SDL_Surface* backlog_cache[256];
    SDL_Surface* prompt_cache[256];

    bool complete_redraw;

    uint64_t backlog_scroll_screen_start;
    SDL_Color backlog_fg_color;
    uint64_t backlog_end_index;
    SDL_Rect backlog_end_point;

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

static void render_char(SDL_Surface* window_surface,
                        Render_State* rend,
                        SDL_Rect* point,
                        SDL_Surface** cache,
                        uint32_t background,
                        SDL_Color foreground,
                        char c) {
    if (c == '\n') {
        point->w = window_surface->w - point->x;
        point->h = rend->font_height;
        SDL_FillRect(window_surface, point, background);

        point->x = 0;
        point->y += rend->font_height;
        return;
    }

    int width = rend->font_width;
    SDL_Surface* s = nullptr;
    if (c == '\t') {
        width *= tab_width;
    } else {
        s = rasterize_character_cached(rend, cache, c, foreground);
    }

    if (point->x + width > window_surface->w) {
        point->x = 0;
        point->y += rend->font_height;
    }
    // Beyond bottom of screen.
    if (point->y >= window_surface->h)
        return;

    {
        ZoneScopedN("blit_character");
        point->w = width;
        point->h = rend->font_height;
        SDL_FillRect(window_surface, point, background);
        if (s)
            SDL_BlitSurface(s, NULL, window_surface, point);
    }

    point->x += width;
}

static void render_backlog(SDL_Surface* window_surface,
                           Render_State* rend,
                           Backlog_State* backlog) {
    ZoneScoped;
    SDL_Rect* point = &rend->backlog_end_point;
    uint64_t i = rend->backlog_end_index;

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

    rend->backlog_end_index = i;
}

static void render_prompt(SDL_Surface* window_surface,
                          Render_State* rend,
                          Prompt_State* prompt,
                          Shell_State* shell) {
    ZoneScoped;

    SDL_Rect point = rend->backlog_end_point;
    if (point.x != 0 || point.y != 0) {
        point.w = window_surface->w - point.x;
        point.h = rend->font_height;
        SDL_FillRect(window_surface, &point, SDL_MapRGB(window_surface->format, 0, 0, 0));
        point.x = 0;
        point.y += rend->font_height;
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
            SDL_Rect fill_rect = {point.x - 1, point.y, 2, rend->font_height};
            uint32_t foreground = SDL_MapRGB(window_surface->format, rend->prompt_fg_color.r,
                                             rend->prompt_fg_color.g, rend->prompt_fg_color.b);
            SDL_FillRect(window_surface, &fill_rect, foreground);

            render_char(window_surface, rend, &point, rend->prompt_cache, background,
                        rend->prompt_fg_color, c);

            if (point.x != 0) {
                uint32_t foreground = SDL_MapRGB(window_surface->format, rend->prompt_fg_color.r,
                                                 rend->prompt_fg_color.g, rend->prompt_fg_color.b);
                SDL_FillRect(window_surface, &fill_rect, foreground);
            }
        } else {
            render_char(window_surface, rend, &point, rend->prompt_cache, background,
                        rend->prompt_fg_color, c);
        }
    }

    SDL_Rect fill_rect = {point.x, point.y, window_surface->w - point.x, rend->font_height};
    SDL_FillRect(window_surface, &fill_rect, background);

    if (prompt->cursor == prompt->text.len) {
        SDL_Rect fill_rect = {point.x - 1, point.y, 2, rend->font_height};
        uint32_t foreground = SDL_MapRGB(window_surface->format, rend->prompt_fg_color.r,
                                         rend->prompt_fg_color.g, rend->prompt_fg_color.b);
        SDL_FillRect(window_surface, &fill_rect, foreground);
    }
}

static void render_frame(SDL_Window* window,
                         Render_State* rend,
                         Backlog_State* backlog,
                         Prompt_State* prompt,
                         Shell_State* shell) {
    ZoneScoped;

    SDL_Surface* window_surface = SDL_GetWindowSurface(window);
    rend->window_height = window_surface->h;

    if (rend->complete_redraw) {
        ZoneScopedN("draw_background");
        SDL_FillRect(window_surface, NULL, SDL_MapRGB(window_surface->format, 0x00, 0x00, 0x00));

        rend->backlog_end_index = rend->backlog_scroll_screen_start;
        rend->backlog_end_point = {};
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

static void scroll_down(Render_State* rend, Backlog_State* backlog, uint64_t lines) {
    uint64_t* line_start = &rend->backlog_scroll_screen_start;
    uint64_t cursor = *line_start;
    while (lines > 0 && cursor < backlog->length) {
        char c = backlog->buffers[OUTER_INDEX(cursor)][INNER_INDEX(cursor)];
        ++cursor;
        if (c == '\n') {
            *line_start = cursor;
            --lines;
        }
    }
}

static void scroll_up(Render_State* rend, Backlog_State* backlog, uint64_t lines) {
    // Scroll up.
    ++lines;
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
}

static uint64_t screen_lines(Render_State* rend) {
    return rend->window_height / rend->font_height;
}

static void ensure_prompt_on_screen(Render_State* rend, Backlog_State* backlog) {
    uint64_t lines = screen_lines(rend);
    if (lines > 3) {
        uint64_t backup = rend->backlog_scroll_screen_start;
        rend->backlog_scroll_screen_start = backlog->length;
        scroll_up(rend, backlog, lines - 3);
        if (rend->backlog_scroll_screen_start > backup)
            rend->complete_redraw = true;
        else
            rend->backlog_scroll_screen_start = backup;
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
            if (event.key.keysym.sym == SDLK_RETURN) {
                append_text(backlog, -1, "\n");
                if (rend->backlog_scroll_screen_start + 1 == backlog->length) {
                    ++rend->backlog_scroll_screen_start;
                    rend->backlog_end_index = rend->backlog_scroll_screen_start;
                    rend->backlog_end_point = {};
                }
                set_backlog_process(backlog, -3);
                append_text(backlog, prompt->process_id, shell->working_directory);
                append_text(backlog, prompt->process_id, " ");
                append_text(backlog, prompt->process_id, prompt->prefix);
                append_text(backlog, -2, prompt->text);
                append_text(backlog, prompt->process_id, "\n");

                if (!run_line(shell, prompt->text, prompt->process_id)) {
                    append_text(backlog, prompt->process_id, "Error: failed to execute\n");
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
            if (mod == KMOD_ALT && event.key.keysym.sym == SDLK_a) {
                prompt->cursor = 0;
                ensure_prompt_on_screen(rend, backlog);
                ++num_events;
            }
            if (mod == KMOD_ALT && event.key.keysym.sym == SDLK_e) {
                prompt->cursor = prompt->text.len;
                ensure_prompt_on_screen(rend, backlog);
                ++num_events;
            }
            if (mod == KMOD_CTRL && event.key.keysym.sym == SDLK_l) {
                rend->backlog_scroll_screen_start = backlog->length;
                rend->complete_redraw = true;
                ++num_events;
            }
            if (mod == KMOD_CTRL && event.key.keysym.sym == SDLK_v) {
                uint64_t lines = (cz::max)(screen_lines(rend), (uint64_t)6) - 3;
                scroll_down(rend, backlog, lines);
                rend->complete_redraw = true;
                ++num_events;
            }
            if (mod == KMOD_ALT && event.key.keysym.sym == SDLK_v) {
                uint64_t lines = (cz::max)(screen_lines(rend), (uint64_t)6) - 3;
                scroll_up(rend, backlog, lines);
                rend->complete_redraw = true;
                ++num_events;
            }
            if (mod == KMOD_ALT && event.key.keysym.sym == SDLK_GREATER) {
                rend->backlog_scroll_screen_start = backlog->length;
                uint64_t lines = (cz::max)(screen_lines(rend), (uint64_t)3) - 3;
                scroll_up(rend, backlog, lines);
                rend->complete_redraw = true;
                ++num_events;
            }
            if (mod == (KMOD_CTRL | KMOD_ALT) && event.key.keysym.sym == SDLK_b) {
                size_t event_index = 0;
                while (event_index < backlog->events.len &&
                       backlog->events[event_index].index < rend->backlog_scroll_screen_start) {
                    ++event_index;
                }
                while (event_index-- > 0) {
                    Backlog_Event* event = &backlog->events[event_index];
                    if (event->type == BACKLOG_EVENT_START_PROMPT) {
                        rend->backlog_scroll_screen_start = backlog->events[event_index].index;
                        rend->complete_redraw = true;
                        ++num_events;
                        break;
                    }
                }
            }
            if (mod == (KMOD_CTRL | KMOD_ALT) && event.key.keysym.sym == SDLK_f) {
                size_t event_index = 0;
                while (event_index < backlog->events.len &&
                       backlog->events[event_index].index <= rend->backlog_scroll_screen_start) {
                    ++event_index;
                }
                for (; event_index < backlog->events.len; ++event_index) {
                    Backlog_Event* event = &backlog->events[event_index];
                    if (event->type == BACKLOG_EVENT_START_PROMPT) {
                        rend->backlog_scroll_screen_start = backlog->events[event_index].index;
                        break;
                    }
                }
                if (event_index >= backlog->events.len)
                    rend->backlog_scroll_screen_start = backlog->length;
                rend->complete_redraw = true;
                ++num_events;
            }
        } break;

        case SDL_TEXTINPUT: {
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
            } else {
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

    float dpi_scale = 1.0f;
    {
        const float dpi_default = 96.0f;
        float dpi = 0;
        if (SDL_GetDisplayDPI(0, &dpi, NULL, NULL) == 0)
            dpi_scale = dpi / dpi_default;
    }

    SDL_Window* window = SDL_CreateWindow(
        "tesh", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 800 * dpi_scale, 800 * dpi_scale,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    CZ_DEFER(SDL_DestroyWindow(window));

    rend.font = open_font(font_path, (int)(font_size * dpi_scale));
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
