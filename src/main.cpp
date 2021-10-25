#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>
#include <stdint.h>
#include <stdio.h>
#include <Tracy.hpp>
#include <cz/defer.hpp>
#include <cz/env.hpp>
#include <cz/format.hpp>
#include <cz/heap.hpp>
#include <cz/process.hpp>
#include <cz/string.hpp>
#include <cz/util.hpp>
#include <cz/vector.hpp>
#include <cz/working_directory.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <SDL_syswm.h>
#include <shellscalingapi.h>
#include <windows.h>
#include "../res/resources.h"
#endif

#include "config.hpp"
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

struct Prompt_State {
    cz::Str prefix;
    cz::String text;
    size_t cursor;
    uint64_t process_id;
    uint64_t history_counter;
    cz::Vector<cz::Str> history;
    cz::Buffer_Array history_arena;
    bool history_searching;
};

///////////////////////////////////////////////////////////////////////////////
// Renderer methods
///////////////////////////////////////////////////////////////////////////////

static void set_icon(SDL_Window* sdl_window) {
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

static bool render_char(SDL_Surface* window_surface,
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

static void render_backlog(SDL_Surface* window_surface,
                           Render_State* rend,
                           Backlog_State* backlog) {
    ZoneScoped;
    Visual_Point* point = &rend->backlog_end;
    uint64_t i = rend->backlog_end.index;

    CZ_ASSERT(point->y >= 0);
    if (point->y >= rend->window_rows_ru)
        return;

    uint64_t process_id = 0;
    SDL_Color bg_color = cfg.process_colors[process_id % cfg.process_colors.len];
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
                    bg_color = cfg.process_colors[process_id % cfg.process_colors.len];
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

        char c = backlog->get(i);
        if (!render_char(window_surface, rend, point, cache, background, fg_color, c))
            break;
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

    SDL_Color bg_color = cfg.process_colors[prompt->process_id % cfg.process_colors.len];
    uint32_t background = SDL_MapRGB(window_surface->format, bg_color.r, bg_color.g, bg_color.b);

    if (shell->active_process == -1) {
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
    } else {
        cz::Str input_prefix = "> ";
        for (size_t i = 0; i < input_prefix.len; ++i) {
            char c = input_prefix[i];
            render_char(window_surface, rend, &point, rend->backlog_cache, background,
                        rend->backlog_fg_color, c);
        }
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
    Visual_Point eol = point;
    render_char(window_surface, rend, &point, rend->backlog_cache, background,
                rend->backlog_fg_color, '\n');

    if (prompt->cursor == prompt->text.len) {
        SDL_Rect fill_rect = {eol.x * rend->font_width - 1, eol.y * rend->font_height, 2,
                              rend->font_height};
        uint32_t foreground = SDL_MapRGB(window_surface->format, rend->prompt_fg_color.r,
                                         rend->prompt_fg_color.g, rend->prompt_fg_color.b);
        SDL_FillRect(window_surface, &fill_rect, foreground);
    }

    if (prompt->history_searching) {
        cz::Str prefix = "HISTORY: ";
        for (size_t i = 0; i < prefix.len; ++i) {
            char c = prefix[i];
            render_char(window_surface, rend, &point, rend->backlog_cache, background,
                        rend->backlog_fg_color, c);
        }
        if (prompt->history_counter < prompt->history.len) {
            cz::Str hist = prompt->history[prompt->history_counter];
            for (size_t i = 0; i < hist.len; ++i) {
                char c = hist[i];
                render_char(window_surface, rend, &point, rend->backlog_cache, background,
                            rend->backlog_fg_color, c);
            }
        }

        render_char(window_surface, rend, &point, rend->backlog_cache, background,
                    rend->backlog_fg_color, '\n');
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
        char c = backlog->get(point.index);
        coord_trans(&point, rend->window_cols, c);
    }

    if (point.y + 3 >= rend->window_rows) {
        rend->backlog_start = {};
        rend->backlog_start.index = prompt_position;
        rend->complete_redraw = true;
        // Reach maximum scroll so stop.
        rend->auto_page = false;
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

    if (rend->auto_page)
        auto_scroll_start_paging(rend, backlog);
    if (rend->auto_scroll)
        ensure_prompt_on_screen(rend, backlog);

    if (shell->active_process != -1)
        ensure_prompt_on_screen(rend, backlog);

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
// Process control
///////////////////////////////////////////////////////////////////////////////

static bool run_line(Shell_State* shell, Backlog_State* backlog, cz::Str text, uint64_t id) {
    cz::Buffer_Array arena = alloc_arena(shell);

#ifdef TRACY_ENABLE
    {
        cz::String message = cz::format(temp_allocator, "Start: ", text);
        TracyMessage(message.buffer, message.len);
    }
#endif

    Parse_Script script = {};
    Error error = parse_script(shell, arena.allocator(), &script, text);
    if (error != Error_Success)
        goto fail;

    error = start_execute_script(shell, backlog, arena, script, text, id);
    if (error == Error_Empty) {
        recycle_arena(shell, arena);
        return true;
    }
    if (error != Error_Success)
        goto fail;

    return true;

fail:;
#ifdef TRACY_ENABLE
    {
        cz::String message = cz::format(temp_allocator, "Failed to start: ", text);
        TracyMessage(message.buffer, message.len);
    }
#endif

    recycle_arena(shell, arena);
    return false;
}

static void run_rc(Shell_State* shell, Backlog_State* backlog) {
    cz::Str home;
    if (!get_env_var(shell, "HOME", &home))
        return;

    cz::Input_File file;
    if (!file.open(cz::format(temp_allocator, home, "/.teshrc").buffer))
        return;
    CZ_DEFER(file.close());

    cz::String contents = {};
    read_to_string(file, temp_allocator, &contents);

    uint64_t id = -1;
    run_line(shell, backlog, contents, id);
}

static void tick_pipeline(Shell_State* shell, Running_Pipeline* pipeline, bool* force_quit) {
    for (size_t p = 0; p < pipeline->pipeline.len; ++p) {
        Running_Program* program = &pipeline->pipeline[p];
        int exit_code = 1;
        if (tick_program(shell, program, &exit_code, force_quit)) {
            if (p + 1 == pipeline->length)
                pipeline->last_exit_code = exit_code;
            pipeline->pipeline.remove(p);
            --p;
        }
        if (*force_quit)
            return;
    }
}

static bool read_process_data(Shell_State* shell,
                              Backlog_State* backlog,
                              Render_State* rend,
                              bool* force_quit) {
    static char buffer[4096];
    size_t starting_length = backlog->length;
    bool changes = false;
    for (size_t i = 0; i < shell->scripts.len; ++i) {
        Running_Script* script = &shell->scripts[i];

        tick_pipeline(shell, &script->fg.pipeline, force_quit);
        if (*force_quit)
            return true;

        if (script->out.is_open()) {
            int64_t result = 0;
            while (1) {
                result = script->out.read_text(buffer, sizeof(buffer), &script->out_carry);
                if (result <= 0)
                    break;
                append_text(backlog, script->id, {buffer, (size_t)result});
            }

            if (result == 0) {
                script->out.close();
                script->out = {};
            }
        }

        if (script->fg.pipeline.pipeline.len == 0) {
#ifdef TRACY_ENABLE
            {
                cz::String message =
                    cz::format(temp_allocator, "End: ", script->fg.pipeline.command_line);
                TracyMessage(message.buffer, message.len);
            }
#endif

            Parse_Line* next = nullptr;
            if (script->fg.pipeline.last_exit_code == 0)
                next = script->fg.on.success;
            else
                next = script->fg.on.failure;

            if (next) {
                // TODO: we shouldn't throw away the arena and then immediately realloc it.
                // It's essentially free we're not even calling `free` but still.  Bad design.
                recycle_pipeline(shell, &script->fg.pipeline);
                script->fg.pipeline = {};

                Error error = start_execute_line(shell, backlog, script, *next);
                if (error != Error_Success && error != Error_Empty) {
                    append_text(backlog, script->id, "Error: failed to execute continuation\n");
                }
            } else {
                // If we're attached then we auto scroll but we can hit an edge case where the
                // final output isn't scrolled to.  So we stop halfway through the output.  I think
                // it would be better if this just called `ensure_prompt_on_screen`.
                if (shell->active_process == script->id)
                    rend->auto_scroll = true;

                recycle_process(shell, script);
                changes = true;
                --i;
            }
        }
    }
    return backlog->length != starting_length || changes;
}

///////////////////////////////////////////////////////////////////////////////
// User events
///////////////////////////////////////////////////////////////////////////////

static void scroll_down(Render_State* rend, Backlog_State* backlog, int lines) {
    Visual_Point* start = &rend->backlog_start;
    int desired_y = start->y + lines;
    while (start->y < desired_y && start->index < backlog->length) {
        char c = backlog->get(start->index);
        coord_trans(start, rend->window_cols, c);
    }
    start->y = 0;
}

static void scroll_up(Render_State* rend, Backlog_State* backlog, int lines) {
    ++lines;
    Visual_Point* line_start = &rend->backlog_start;
    uint64_t cursor = line_start->index;
    uint64_t line_chars = 0;
    uint64_t backwards_tab = 0;
    while (lines > 0 && cursor > 0) {
        --cursor;
        char c = backlog->get(cursor);
        if (backwards_tab == 0) {
            line_chars++;
        } else {
            backwards_tab--;
        }

        if (c == '\n') {
            line_start->index = cursor + 1;
            --lines;
            line_chars = 0;
            backwards_tab = 0;
        } else if (c == '\t') {
            line_chars--;
            if (line_chars + (8 - (line_chars % 8)) > rend->window_cols) {
                ++cursor;  // Don't consume the tab, it was actually on the previous line
                --lines;
                line_chars = 0;
                backwards_tab = 0;
                continue;
            }
            backwards_tab = (8 - (line_chars % 8));
            line_chars += backwards_tab;
        }
        if (line_chars == rend->window_cols) {
            line_start->index = cursor + 1;
            --lines;
            line_chars = 0;
            backwards_tab = 0;
        }
    }
    line_start->y = 0;
    line_start->x = 0;
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

static void resolve_history_searching(Prompt_State* prompt) {
    if (prompt->history_searching) {
        prompt->history_searching = false;
        prompt->text.len = 0;
        if (prompt->history_counter < prompt->history.len) {
            cz::Str hist = prompt->history[prompt->history_counter];
            prompt->text.reserve(cz::heap_allocator(), hist.len);
            prompt->text.append(hist);
            prompt->cursor = prompt->text.len;
        }
    }
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
            if ((mod == KMOD_CTRL && event.key.keysym.sym == SDLK_c) ||
                event.key.keysym.sym == SDLK_RETURN) {
                rend->auto_page = false;
                rend->auto_scroll = true;

                resolve_history_searching(prompt);

                Running_Script* script = active_process(shell);
                if (script) {
                    set_backlog_process(backlog, script->id);
                } else {
                    append_text(backlog, -1, "\n");
                    if (rend->backlog_start.index + 1 == backlog->length) {
                        ++rend->backlog_start.index;
                        rend->backlog_end = rend->backlog_start;
                    }

                    set_backlog_process(backlog, -3);
                    append_text(backlog, prompt->process_id, shell->working_directory);
                    append_text(backlog, prompt->process_id, " ");
                    append_text(backlog, prompt->process_id, prompt->prefix);
                }
                append_text(backlog, -2, prompt->text);
                append_text(backlog, prompt->process_id, "\n");

                if (event.key.keysym.sym == SDLK_RETURN) {
                    if (script) {
                        (void)script->in.write(prompt->text);
                        --prompt->process_id;
                    } else {
                        rend->auto_page = cfg.on_spawn_auto_page;
                        rend->auto_scroll = cfg.on_spawn_auto_scroll;
                        if (!run_line(shell, backlog, prompt->text, prompt->process_id)) {
                            append_text(backlog, prompt->process_id, "Error: failed to execute\n");
                        }
                    }
                } else {
                    if (script) {
#ifdef TRACY_ENABLE
                        cz::String message =
                            cz::format(temp_allocator, "End: ", script->fg.pipeline.command_line);
                        TracyMessage(message.buffer, message.len);
#endif

                        recycle_process(shell, script);
                    }
                }

                if (prompt->text.len > 0) {
                    if (prompt->history.len == 0 || prompt->history.last() != prompt->text) {
                        prompt->history.reserve(cz::heap_allocator(), 1);
                        prompt->history.push(prompt->text.clone(prompt->history_arena.allocator()));
                        prompt->history_counter = prompt->history.len;
                    }
                }

                prompt->text.len = 0;
                prompt->cursor = 0;
                ++prompt->process_id;

                ensure_prompt_on_screen(rend, backlog);
                ++num_events;
            }
            if (mod == KMOD_CTRL && event.key.keysym.sym == SDLK_z) {
                rend->auto_page = false;
                rend->auto_scroll = true;
                if (shell->active_process == -1) {
                    if (shell->scripts.len > 0)
                        shell->active_process = shell->scripts.last().id;
                } else {
                    shell->active_process = -1;
                }
                ++num_events;
            }
            if (mod == 0 && event.key.keysym.sym == SDLK_BACKSPACE) {
                rend->auto_page = false;
                rend->auto_scroll = true;
                if (prompt->cursor > 0) {
                    --prompt->cursor;
                    prompt->text.remove(prompt->cursor);
                }
                ensure_prompt_on_screen(rend, backlog);
                ++num_events;
            }
            if (mod == (KMOD_CTRL | KMOD_ALT) && event.key.keysym.sym == SDLK_BACKSPACE) {
                rend->auto_page = false;
                rend->auto_scroll = true;
                prompt->text.remove_range(0, prompt->cursor);
                prompt->cursor = 0;
                ensure_prompt_on_screen(rend, backlog);
                ++num_events;
            }
            if ((mod == 0 && event.key.keysym.sym == SDLK_DELETE) ||
                (mod == KMOD_CTRL && event.key.keysym.sym == SDLK_d)) {
                rend->auto_page = false;
                rend->auto_scroll = true;
                if (prompt->cursor < prompt->text.len) {
                    prompt->text.remove(prompt->cursor);
                }
                ensure_prompt_on_screen(rend, backlog);
                ++num_events;
            }
            if ((mod == 0 && event.key.keysym.sym == SDLK_LEFT) ||
                (mod == KMOD_CTRL && event.key.keysym.sym == SDLK_b)) {
                rend->auto_page = false;
                rend->auto_scroll = true;
                if (prompt->cursor > 0) {
                    --prompt->cursor;
                }
                ensure_prompt_on_screen(rend, backlog);
                ++num_events;
            }
            if ((mod == 0 && event.key.keysym.sym == SDLK_RIGHT) ||
                (mod == KMOD_CTRL && event.key.keysym.sym == SDLK_f)) {
                rend->auto_page = false;
                rend->auto_scroll = true;
                if (prompt->cursor < prompt->text.len) {
                    ++prompt->cursor;
                }
                ensure_prompt_on_screen(rend, backlog);
                ++num_events;
            }
            if ((mod == 0 && event.key.keysym.sym == SDLK_UP) ||
                (mod == KMOD_ALT && event.key.keysym.sym == SDLK_p)) {
                rend->auto_page = false;
                rend->auto_scroll = true;
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
                rend->auto_page = false;
                rend->auto_scroll = true;
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
            if (mod == KMOD_CTRL && event.key.keysym.sym == SDLK_r) {
                rend->auto_page = false;
                rend->auto_scroll = true;
                if (!prompt->history_searching) {
                    prompt->history_searching = true;
                    prompt->history_counter = prompt->history.len;
                }
                while (1) {
                    if (prompt->history_counter == 0) {
                        prompt->history_counter = prompt->history.len;
                        break;
                    }
                    --prompt->history_counter;
                    cz::Str hist = prompt->history[prompt->history_counter];
                    if (hist.contains_case_insensitive(prompt->text))
                        break;
                }
                ensure_prompt_on_screen(rend, backlog);
                ++num_events;
            }
            if (mod == KMOD_ALT && event.key.keysym.sym == SDLK_r) {
                rend->auto_page = false;
                rend->auto_scroll = true;
                if (prompt->history_searching) {
                    while (1) {
                        ++prompt->history_counter;
                        if (prompt->history_counter >= prompt->history.len) {
                            prompt->history_counter = prompt->history.len;
                            prompt->history_searching = false;
                            break;
                        }
                        cz::Str hist = prompt->history[prompt->history_counter];
                        if (hist.contains_case_insensitive(prompt->text))
                            break;
                    }
                }
                ensure_prompt_on_screen(rend, backlog);
                ++num_events;
            }
            if (mod == KMOD_CTRL && event.key.keysym.sym == SDLK_g) {
                rend->auto_page = false;
                rend->auto_scroll = true;
                resolve_history_searching(prompt);
                ensure_prompt_on_screen(rend, backlog);
                ++num_events;
            }
            if ((mod == 0 && event.key.keysym.sym == SDLK_HOME) ||
                (mod == KMOD_ALT && event.key.keysym.sym == SDLK_a)) {
                rend->auto_page = false;
                rend->auto_scroll = true;
                prompt->cursor = 0;
                ensure_prompt_on_screen(rend, backlog);
                ++num_events;
            }
            if ((mod == 0 && event.key.keysym.sym == SDLK_END) ||
                (mod == KMOD_ALT && event.key.keysym.sym == SDLK_e)) {
                rend->auto_page = false;
                rend->auto_scroll = true;
                prompt->cursor = prompt->text.len;
                ensure_prompt_on_screen(rend, backlog);
                ++num_events;
            }
            if ((mod == KMOD_CTRL && event.key.keysym.sym == SDLK_LEFT) ||
                (mod == KMOD_ALT && event.key.keysym.sym == SDLK_LEFT) ||
                (mod == KMOD_ALT && event.key.keysym.sym == SDLK_b)) {
                rend->auto_page = false;
                rend->auto_scroll = true;
                while (prompt->cursor > 0) {
                    if (cz::is_alpha(prompt->text[prompt->cursor - 1]))
                        break;
                    --prompt->cursor;
                }
                while (prompt->cursor > 0) {
                    if (!cz::is_alpha(prompt->text[prompt->cursor - 1]))
                        break;
                    --prompt->cursor;
                }
                ensure_prompt_on_screen(rend, backlog);
                ++num_events;
            }
            if ((mod == KMOD_CTRL && event.key.keysym.sym == SDLK_RIGHT) ||
                (mod == KMOD_ALT && event.key.keysym.sym == SDLK_RIGHT) ||
                (mod == KMOD_ALT && event.key.keysym.sym == SDLK_f)) {
                rend->auto_page = false;
                rend->auto_scroll = true;
                while (prompt->cursor < prompt->text.len) {
                    if (cz::is_alpha(prompt->text[prompt->cursor]))
                        break;
                    ++prompt->cursor;
                }
                while (prompt->cursor < prompt->text.len) {
                    if (!cz::is_alpha(prompt->text[prompt->cursor]))
                        break;
                    ++prompt->cursor;
                }
                ensure_prompt_on_screen(rend, backlog);
                ++num_events;
            }
            if (mod == KMOD_CTRL && event.key.keysym.sym == SDLK_l) {
                rend->auto_page = false;
                rend->auto_scroll = true;
                rend->backlog_start = {};
                rend->backlog_start.index = backlog->length;
                rend->complete_redraw = true;
                ++num_events;
            }
            if ((mod == 0 && event.key.keysym.sym == SDLK_PAGEDOWN) ||
                (mod == KMOD_CTRL && event.key.keysym.sym == SDLK_v)) {
                rend->auto_page = false;
                rend->auto_scroll = false;
                shell->active_process = -1;
                int lines = cz::max(rend->window_rows, 6) - 3;
                scroll_down(rend, backlog, lines);
                rend->complete_redraw = true;
                ++num_events;
            }
            if ((mod == 0 && event.key.keysym.sym == SDLK_PAGEUP) ||
                (mod == KMOD_ALT && event.key.keysym.sym == SDLK_v)) {
                rend->auto_page = false;
                rend->auto_scroll = false;
                shell->active_process = -1;
                int lines = cz::max(rend->window_rows, 6) - 3;
                scroll_up(rend, backlog, lines);
                rend->complete_redraw = true;
                ++num_events;
            }
            if (mod == KMOD_ALT && event.key.keysym.sym == SDLK_GREATER) {
                rend->auto_page = false;
                rend->auto_scroll = true;
                rend->backlog_start = {};
                rend->backlog_start.index = backlog->length;
                int lines = cz::max(rend->window_rows, 3) - 3;
                scroll_up(rend, backlog, lines);
                rend->complete_redraw = true;
                ++num_events;
            }
            if (mod == KMOD_ALT && event.key.keysym.sym == SDLK_LESS) {
                rend->auto_page = false;
                rend->auto_scroll = false;
                shell->active_process = -1;
                size_t event_index = backlog->events.len;
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
            if (mod == (KMOD_CTRL | KMOD_ALT) && event.key.keysym.sym == SDLK_b) {
                rend->auto_page = false;
                rend->auto_scroll = false;
                shell->active_process = -1;
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
                rend->auto_page = false;
                rend->auto_scroll = false;
                shell->active_process = -1;
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
                rend->auto_page = false;
                rend->auto_scroll = true;
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

            // Note: C-= used to zoom in so you don't have to hold shift.
            if (mod == KMOD_CTRL &&
                (event.key.keysym.sym == SDLK_EQUALS || event.key.keysym.sym == SDLK_MINUS)) {
                int new_font_size = rend->font_size;
                if (event.key.keysym.sym == SDLK_EQUALS) {
                    new_font_size += 4;
                } else {
                    new_font_size = cz::max(new_font_size - 4, 4);
                }

                TTF_Font* new_font =
                    open_font(cfg.font_path, (int)(new_font_size * rend->dpi_scale));
                if (new_font) {
                    close_font(rend);

                    rend->font = new_font;
                    rend->font_size = new_font_size;
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
            rend->auto_page = false;
            rend->auto_scroll = true;

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
            rend->auto_page = false;
            rend->auto_scroll = false;
            shell->active_process = -1;

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
// History control
///////////////////////////////////////////////////////////////////////////////

uint64_t history_counter;
cz::Vector<cz::Str> history;
cz::Buffer_Array history_arena;

static void load_history(Prompt_State* prompt, Shell_State* shell) {
    cz::Str home = {};
    if (!get_env_var(shell, "HOME", &home))
        return;

    cz::String path = cz::format(temp_allocator, home, "/.tesh_history");

    cz::Input_File file;
    if (!file.open(path.buffer))
        return;
    CZ_DEFER(file.close());

    cz::String buffer = {};
    buffer.reserve_exact(temp_allocator, 4096);

    cz::String element = {};
    while (1) {
        int64_t result = file.read(buffer.buffer, buffer.cap);
        if (result <= 0)
            break;
        buffer.len = result;

        cz::Str remaining = buffer;
        while (remaining.len > 0) {
            cz::Str before = remaining, after = {};
            bool flush = remaining.split_excluding('\n', &before, &after);
            element.reserve_exact(prompt->history_arena.allocator(), before.len);
            element.append(before);
            if (flush && element.len > 0) {
                prompt->history.reserve(cz::heap_allocator(), 1);
                prompt->history.push(element);
                element = {};
            }
            remaining = after;
        }
    }

    if (element.len > 0) {
        prompt->history.reserve(cz::heap_allocator(), 1);
        prompt->history.push(element);
    }

    prompt->history_counter = prompt->history.len;
}

static void save_history(Prompt_State* prompt, Shell_State* shell) {
    cz::Str home = {};
    if (!get_env_var(shell, "HOME", &home))
        return;

    cz::String path = cz::format(temp_allocator, home, "/.tesh_history");

    cz::Output_File file;
    if (!file.open(path.buffer))
        return;
    CZ_DEFER(file.close());

    cz::String buffer = {};
    buffer.reserve_exact(temp_allocator, 4096);

    for (size_t i = 0; i < prompt->history.len; ++i) {
        cz::Str element = prompt->history[i];
        if (element.len + 1 > buffer.remaining()) {
            if (file.write(buffer) != buffer.len)
                return;
            buffer.len = 0;

            // A truly massive line shouldn't be double buffered.
            if (element.len + 1 > buffer.cap) {
                if (file.write(element) != element.len)
                    return;
                buffer.push('\n');
                continue;
            }
        }

        buffer.append(element);
        buffer.push('\n');
    }

    (void)file.write(buffer);
}

///////////////////////////////////////////////////////////////////////////////
// configuration methods
///////////////////////////////////////////////////////////////////////////////

static void load_default_configuration() {
    cfg.on_spawn_attach = false;
    cfg.on_spawn_auto_page = true;
    cfg.on_spawn_auto_scroll = false;

#ifdef _WIN32
    cfg.font_path = "C:/Windows/Fonts/MesloLGM-Regular.ttf";
#else
    cfg.font_path = "/usr/share/fonts/TTF/MesloLGMDZ-Regular.ttf";
#endif
    cfg.default_font_size = 12;
    cfg.tab_width = 8;

    static SDL_Color process_colors[] = {
        {0x18, 0, 0, 0xff},    {0, 0x18, 0, 0xff},    {0, 0, 0x26, 0xff},
        {0x11, 0x11, 0, 0xff}, {0, 0x11, 0x11, 0xff}, {0x11, 0, 0x17, 0xff},
    };
    cfg.process_colors = process_colors;
}

static void load_environment_variables(Shell_State* shell) {
#ifdef _WIN32
    {
        char* penv = GetEnvironmentStringsA();
        if (!penv)
            return;
        CZ_DEFER(FreeEnvironmentStringsA(penv));

        const char* iter = penv;
        while (*iter) {
            cz::Str line = iter;
            iter += line.len + 1;

            cz::Str key, value;
            if (line.split_excluding('=', &key, &value)) {
                // Windows special environment variables have
                // a = as the first character so ignore those.
                if (key.len > 0)
                    set_env_var(shell, key, value);
            }
        }
    }

    // Set HOME to the user home directory.
    cz::Str temp;
    if (!get_env_var(shell, "HOME", &temp)) {
        cz::String home = {};
        if (cz::env::get_home(cz::heap_allocator(), &home)) {
            set_env_var(shell, "HOME", home);
        }
    }
#else
    extern char** environ;
    for (char** iter = environ; *iter; ++iter) {
        cz::Str line = *iter;
        cz::Str key, value;
        if (line.split_excluding('=', &key, &value)) {
            if (key.len > 0)
                set_env_var(shell, key, value);
        }
    }
#endif
}

///////////////////////////////////////////////////////////////////////////////
// main
///////////////////////////////////////////////////////////////////////////////

int actual_main(int argc, char** argv) {
    Render_State rend = {};
    Backlog_State backlog = {};
    Prompt_State prompt = {};
    Shell_State shell = {};

    load_default_configuration();

    prompt.history_arena.init();

    cz::Buffer_Array permanent_arena;
    permanent_arena.init();
    permanent_allocator = permanent_arena.allocator();

    cz::Buffer_Array temp_arena;
    temp_arena.init();
    temp_allocator = temp_arena.allocator();

    CZ_DEFER(cleanup_processes(&shell));

    set_program_name(/*fallback=*/argv[0]);
    set_program_directory();

    prompt.prefix = "$ ";
    rend.complete_redraw = true;

    rend.font_size = cfg.default_font_size;

    if (!cz::get_working_directory(cz::heap_allocator(), &shell.working_directory)) {
        fprintf(stderr, "Failed to get working directory\n");
        return 1;
    }

    load_environment_variables(&shell);

    load_history(&prompt, &shell);
    CZ_DEFER(save_history(&prompt, &shell));

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

    int img_init_flags = IMG_INIT_PNG;
    if (IMG_Init(img_init_flags) != img_init_flags) {
        fprintf(stderr, "IMG_Init failed: %s\n", IMG_GetError());
        return 1;
    }
    CZ_DEFER(IMG_Quit());

    rend.dpi_scale = 1.0f;
    {
        const float dpi_default = 96.0f;
        float dpi = 0;
        if (SDL_GetDisplayDPI(0, &dpi, NULL, NULL) == 0)
            rend.dpi_scale = dpi / dpi_default;
    }

    SDL_Window* window =
        SDL_CreateWindow("tesh", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                         (int)(800 * rend.dpi_scale), (int)(800 * rend.dpi_scale),
                         SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    CZ_DEFER(SDL_DestroyWindow(window));

    set_icon(window);

    rend.font = open_font(cfg.font_path, (int)(rend.font_size * rend.dpi_scale));
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

    run_rc(&shell, &backlog);

    while (1) {
        uint32_t start_frame = SDL_GetTicks();

        temp_arena.clear();

        try {
            int status = process_events(&backlog, &prompt, &rend, &shell);
            if (status < 0)
                break;

            bool force_quit = false;
            if (read_process_data(&shell, &backlog, &rend, &force_quit))
                status = 1;

            if (force_quit)
                break;

            if (status > 0)
                render_frame(window, &rend, &backlog, &prompt, &shell);
        } catch (cz::PanicReachedException& ex) {
            fprintf(stderr, "Fatal error: %s\n", ex.what());
            return 1;
        }

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
