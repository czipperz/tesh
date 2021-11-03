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
#include <shellscalingapi.h>
#include <windows.h>
#include "../res/resources.h"
#endif

#include "backlog.hpp"
#include "config.hpp"
#include "global.hpp"
#include "render.hpp"
#include "shell.hpp"

void resize_font(int font_size, Render_State* rend);
static Backlog_State* push_backlog(cz::Vector<Backlog_State*>* backlogs, uint64_t id);
static void scroll_down1(Render_State* rend, cz::Slice<Backlog_State*> backlogs, int lines);

///////////////////////////////////////////////////////////////////////////////
// Type definitions
///////////////////////////////////////////////////////////////////////////////

struct Prompt_State {
    cz::Str prefix;
    cz::String text;
    size_t cursor;
    uint64_t process_id;
    uint64_t history_counter;
    cz::Vector<cz::Str> history;
    cz::Vector<cz::Str> stdin_history;
    cz::Buffer_Array history_arena;
    bool history_searching;
};

static cz::Vector<cz::Str>* prompt_history(Prompt_State* prompt, bool script);

///////////////////////////////////////////////////////////////////////////////
// Renderer methods
///////////////////////////////////////////////////////////////////////////////

static void render_info(SDL_Surface* window_surface,
                        Render_State* rend,
                        Visual_Point point,
                        uint32_t background,
                        cz::Str info) {
    point.y--;
    point.x = (int)(rend->window_cols - info.len);
    for (size_t i = 0; i < info.len; ++i) {
        if (!render_char(window_surface, rend, &point, rend->prompt_cache, background,
                         rend->prompt_fg_color, info[i]))
            break;
    }
}

static void render_backlog(SDL_Surface* window_surface,
                           Render_State* rend,
                           Shell_State* shell,
                           std::chrono::high_resolution_clock::time_point now,
                           Backlog_State* backlog) {
    ZoneScoped;
    Visual_Point* point = &rend->backlog_end;
    uint64_t i = 0;
    if (point->outer == backlog->id)
        i = point->inner;

    CZ_ASSERT(point->y >= 0);
    if (point->y >= rend->window_rows_ru)
        return;

    std::chrono::high_resolution_clock::time_point end = backlog->end;
    if (!backlog->done) {
        if (!lookup_process(shell, backlog->id)) {
            backlog->done = true;
            backlog->end = now;
        }
        end = now;
    }
    unsigned millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - backlog->start).count();
    cz::Str info = cz::asprintf(temp_allocator, "%u.%.3us", millis / 1000, millis % 1000);

    uint64_t process_id = backlog->id;
    SDL_Color bg_color = cfg.process_colors[process_id % cfg.process_colors.len];
    if (shell->active_process == process_id) {
        bg_color.r *= 2;
        bg_color.g *= 2;
        bg_color.b *= 2;
    }
    uint32_t background = SDL_MapRGB(window_surface->format, bg_color.r, bg_color.g, bg_color.b);

    int original_window_cols = rend->window_cols;
    int original_y = point->y;
    bool original_test = false;
    uint32_t original_background = background;
    if (info.len + 4 < rend->window_cols) {
        rend->window_cols -= info.len + 4;
        original_test = true;
    }

    SDL_Surface** cache = rend->backlog_cache;
    SDL_Color fg_color = rend->backlog_fg_color;

    size_t event_index = 0;

    for (; i < backlog->length; ++i) {
        while (event_index < backlog->events.len && backlog->events[event_index].index <= i) {
            Backlog_Event* event = &backlog->events[event_index];
            if (event->type == BACKLOG_EVENT_START_PROCESS) {
                cache = rend->backlog_cache;
                fg_color = rend->backlog_fg_color;
            } else if (event->type == BACKLOG_EVENT_START_INPUT) {
                cache = rend->prompt_cache;
                fg_color = rend->prompt_fg_color;
            } else {
                CZ_PANIC("unreachable");
            }
            ++event_index;
        }

        char c = backlog->get(i);
        if (!render_char(window_surface, rend, point, cache, background, fg_color, c))
            break;

        if (original_test && point->y != original_y) {
            original_test = false;
            rend->window_cols = original_window_cols;
            render_info(window_surface, rend, *point, original_background, info);
        }
    }

    rend->backlog_end.outer = backlog->id;
    rend->backlog_end.inner = i;

    if (rend->backlog_end.inner == backlog->length && backlog->length > 0 &&
        backlog->get(backlog->length - 1) != '\n' &&
        !render_char(window_surface, rend, point, rend->backlog_cache, background,
                     rend->prompt_fg_color, '\n'))
        return;

    if (original_test && point->y != original_y) {
        original_test = false;
        rend->window_cols = original_window_cols;
        render_info(window_surface, rend, *point, original_background, info);
    }

    bg_color = {};
    background = SDL_MapRGB(window_surface->format, bg_color.r, bg_color.g, bg_color.b);
    if (!render_char(window_surface, rend, point, rend->backlog_cache, background,
                     rend->prompt_fg_color, '\n'))
        return;

    if (original_test && point->y != original_y) {
        original_test = false;
        rend->window_cols = original_window_cols;
        render_info(window_surface, rend, *point, original_background, info);
    }
}

static void render_backlogs(SDL_Surface* window_surface,
                            Render_State* rend,
                            Shell_State* shell,
                            std::chrono::high_resolution_clock::time_point now,
                            cz::Slice<Backlog_State*> backlogs) {
    for (size_t i = rend->backlog_start.outer; i < backlogs.len; ++i) {
        render_backlog(window_surface, rend, shell, now, backlogs[i]);
    }
}

static void render_prompt(SDL_Surface* window_surface,
                          Render_State* rend,
                          Prompt_State* prompt,
                          Shell_State* shell) {
    ZoneScoped;

    Visual_Point point = rend->backlog_end;

    uint64_t process_id =
        (shell->active_process == -1 ? prompt->process_id : shell->active_process);
    SDL_Color bg_color = cfg.process_colors[process_id % cfg.process_colors.len];
    bg_color.r *= 2;
    bg_color.g *= 2;
    bg_color.b *= 2;
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
        cz::Vector<cz::Str>* history = prompt_history(prompt, shell->active_process != -1);
        if (prompt->history_counter < history->len) {
            cz::Str hist = history->get(prompt->history_counter);
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

static void ensure_prompt_on_screen(Render_State* rend, cz::Slice<Backlog_State*> backlogs);
static void auto_scroll_start_paging(Render_State* rend, cz::Slice<Backlog_State*> backlogs) {
    if (rend->window_rows <= 3)
        return;

    Visual_Point backup = rend->backlog_start;

    // If we put the previous prompt at the top what happens.
    Visual_Point top_prompt = {};
    top_prompt = {};
    top_prompt.outer = (backlogs.len > 0 ? backlogs.len - 1 : 0);

    rend->backlog_start = top_prompt;
    scroll_down1(rend, backlogs, rend->window_rows - 3);

    if (rend->backlog_start.y + 3 >= rend->window_rows) {
        // More than one page of content so stop auto paging.
        rend->backlog_start = top_prompt;
        rend->complete_redraw = true;
        rend->auto_page = false;
    } else {
        rend->backlog_start = backup;
        ensure_prompt_on_screen(rend, backlogs);
    }
}

static void render_frame(SDL_Window* window,
                         Render_State* rend,
                         cz::Slice<Backlog_State*> backlogs,
                         Prompt_State* prompt,
                         Shell_State* shell) {
    ZoneScoped;

    std::chrono::high_resolution_clock::time_point now = std::chrono::high_resolution_clock::now();

    SDL_Surface* window_surface = SDL_GetWindowSurface(window);
    rend->window_rows = window_surface->h / rend->font_height;
    rend->window_rows_ru = (window_surface->h + rend->font_height - 1) / rend->font_height;
    rend->window_cols = window_surface->w / rend->font_width;

    if (rend->auto_page)
        auto_scroll_start_paging(rend, backlogs);
    if (rend->auto_scroll)
        ensure_prompt_on_screen(rend, backlogs);

    if (shell->active_process != -1)
        ensure_prompt_on_screen(rend, backlogs);

    // TODO remove this
    rend->complete_redraw = true;

    if (rend->complete_redraw) {
        ZoneScopedN("draw_background");
        SDL_FillRect(window_surface, NULL, SDL_MapRGB(window_surface->format, 0x00, 0x00, 0x00));
        rend->backlog_end = rend->backlog_start;
    }

    render_backlogs(window_surface, rend, shell, now, backlogs);
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

static bool run_script(Shell_State* shell, Backlog_State* backlog, cz::Str text) {
    cz::Buffer_Array arena = alloc_arena(shell);

#ifdef TRACY_ENABLE
    {
        cz::String message = cz::format(temp_allocator, "Start: ", text);
        TracyMessage(message.buffer, message.len);
    }
#endif

    Parse_Script script = {};
    Error error = parse_script(shell, arena.allocator(), &script, {}, text);
    if (error != Error_Success)
        goto fail;

    error = start_execute_script(shell, backlog, arena, script, text);
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
    if (!get_var(shell, "HOME", &home))
        return;

    cz::Input_File file;
    if (!file.open(cz::format(temp_allocator, home, "/.teshrc").buffer))
        return;
    CZ_DEFER(file.close());

    cz::String contents = {};
    read_to_string(file, temp_allocator, &contents);

    run_script(shell, backlog, contents);
}

static void tick_pipeline(Shell_State* shell,
                          Render_State* rend,
                          cz::Slice<Backlog_State*> backlogs,
                          Backlog_State* backlog,
                          Running_Script* script,
                          Running_Line* line,
                          bool* force_quit) {
    Running_Pipeline* pipeline = &line->pipeline;
    for (size_t p = 0; p < pipeline->pipeline.len; ++p) {
        Running_Program* program = &pipeline->pipeline[p];
        int exit_code = 1;
        if (tick_program(shell, rend, backlogs, backlog, script, line, program, &exit_code,
                         force_quit)) {
            if (p + 1 == pipeline->length)
                pipeline->last_exit_code = exit_code;
            pipeline->pipeline.remove(p);
            --p;
            if (pipeline->pipeline.len == 0)
                return;
        }
        if (*force_quit)
            return;
    }
}

static bool finish_line(Shell_State* shell,
                        Backlog_State* backlog,
                        Running_Script* script,
                        Running_Line* line,
                        bool background) {
#ifdef TRACY_ENABLE
    {
        cz::String message = cz::format(temp_allocator, "End: ", line->pipeline.command_line);
        TracyMessage(message.buffer, message.len);
    }
#endif

    Parse_Line* next = nullptr;
    if (line->pipeline.last_exit_code == 0)
        next = line->on.success;
    else
        next = line->on.failure;

    if (!next)
        return false;

    // TODO: we shouldn't throw away the arena and then immediately realloc it.
    // It's essentially free we're not even calling `free` but still.  Bad design.
    recycle_pipeline(shell, &line->pipeline);

    if (background) {
        script->bg.remove(line - script->bg.elems);
    } else {
        line->pipeline = {};
    }

    Error error = start_execute_line(shell, backlog, script, *next, background);
    if (error != Error_Success && error != Error_Empty) {
        append_text(backlog, "Error: failed to execute continuation\n");
    }

    return true;
}

static void finish_script(Shell_State* shell,
                          Backlog_State* backlog,
                          Render_State* rend,
                          Running_Script* script) {
    // If we're attached then we auto scroll but we can hit an edge case where the
    // final output isn't scrolled to.  So we stop halfway through the output.  I
    // think it would be better if this just called `ensure_prompt_on_screen`.
    if (shell->active_process == script->id)
        rend->auto_scroll = true;

    recycle_process(shell, script);
}

static bool read_process_data(Shell_State* shell,
                              cz::Slice<Backlog_State*> backlogs,
                              Render_State* rend,
                              bool* force_quit) {
    static char buffer[4096];
    bool changes = false;
    for (size_t i = 0; i < shell->scripts.len; ++i) {
        Running_Script* script = &shell->scripts[i];
        Backlog_State* backlog = backlogs[script->id];
        size_t starting_length = backlog->length;

        for (size_t b = 0; b < script->bg.len; ++b) {
            Running_Line* line = &script->bg[b];
            tick_pipeline(shell, rend, backlogs, backlog, script, line, force_quit);
            if (line->pipeline.pipeline.len == 0) {
                finish_line(shell, backlog, script, line, /*background=*/true);
                --b;
            }
        }

        tick_pipeline(shell, rend, backlogs, backlog, script, &script->fg, force_quit);

        if (*force_quit)
            return true;

        if (script->out.is_open()) {
            int64_t result = 0;
            while (1) {
                result = script->out.read_text(buffer, sizeof(buffer), &script->out_carry);
                if (result <= 0)
                    break;
                append_text(backlog, {buffer, (size_t)result});
            }

            if (result == 0) {
                script->out.close();
                script->out = {};
            }
        }

        if (script->fg.pipeline.pipeline.len == 0) {
            bool started = finish_line(shell, backlog, script, &script->fg, /*background=*/false);
            if (started) {
                // Rerun to prevent long scripts from only doing one command per frame.
                // TODO: rate limit to prevent big scripts (with all builtins) from hanging.
                --i;
            } else {
                script->fg_finished = true;
                --i;
            }
        }

        if (script->fg_finished && script->bg.len == 0) {
            finish_script(shell, backlog, rend, script);
            changes = true;
            --i;
        }

        if (backlog->length != starting_length)
            changes = true;
    }
    return changes;
}

///////////////////////////////////////////////////////////////////////////////
// User events
///////////////////////////////////////////////////////////////////////////////

static void scroll_down1(Render_State* rend, cz::Slice<Backlog_State*> backlogs, int lines) {
    Visual_Point* start = &rend->backlog_start;
    if (start->outer < backlogs.len) {
        int desired_y = start->y + lines;
        Backlog_State* backlog = backlogs[start->outer];
        while (1) {
            if (start->inner >= backlog->length) {
                if (start->inner == backlog->length && backlog->length > 0 &&
                    backlog->get(backlog->length - 1) != '\n') {
                    coord_trans(start, rend->window_cols, '\n');
                    if (start->y >= desired_y)
                        break;
                }

                coord_trans(start, rend->window_cols, '\n');

                start->outer++;
                start->inner = 0;
                if (start->outer == backlogs.len)
                    break;
                backlog = backlogs[start->outer];

                if (start->y >= desired_y)
                    break;
                continue;
            }

            Visual_Point sp = *start;
            char c = backlog->get(start->inner);
            coord_trans(start, rend->window_cols, c);
            if (start->y >= desired_y) {
                if (start->x > 0) {
                    *start = sp;
                    start->y++;
                    start->x = 0;
                }
                break;
            }
        }
    }
}
static void scroll_down(Render_State* rend, cz::Slice<Backlog_State*> backlogs, int lines) {
    scroll_down1(rend, backlogs, lines);
    rend->backlog_start.y = 0;
}

static void scroll_up(Render_State* rend, cz::Slice<Backlog_State*> backlogs, int lines) {
    Visual_Point* line_start = &rend->backlog_start;
    Backlog_State* backlog = backlogs[line_start->outer];
    uint64_t cursor = line_start->inner;
    uint64_t line_chars = 0;
    uint64_t backwards_tab = 0;
    bool first = true;
    while (lines > 0) {
        if (cursor == 0) {
            if (line_start->outer == 0)
                break;
            line_start->outer--;
            backlog = backlogs[line_start->outer];
            cursor = backlog->length;
            if (backlog->length > 0 && backlog->get(backlog->length - 1) != '\n') {
                cursor++;
            }
            cursor++;
        }

        cursor--;

        if (first) {
            first = false;
            continue;
        }

        char c = (cursor >= backlog->length ? '\n' : backlog->get(cursor));
        if (backwards_tab == 0) {
            line_chars++;
        } else {
            backwards_tab--;
        }

        if (c == '\n') {
            line_start->inner = cursor + 1;
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
            line_start->inner = cursor + 1;
            --lines;
            line_chars = 0;
            backwards_tab = 0;
        }
    }
    line_start->y = 0;
    line_start->x = 0;
}

static void ensure_prompt_on_screen(Render_State* rend, cz::Slice<Backlog_State*> backlogs) {
    if (rend->window_rows > 3) {
        Visual_Point backup = rend->backlog_start;
        rend->backlog_start = {};
        rend->backlog_start.outer = backlogs.len;
        scroll_up(rend, backlogs, rend->window_rows - 3);
        if (rend->backlog_start.outer > backup.outer)
            rend->complete_redraw = true;
        else if (rend->backlog_start.outer == backup.outer &&
                 rend->backlog_start.inner > backup.inner)
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

static void resolve_history_searching(Prompt_State* prompt, cz::Vector<cz::Str>* history) {
    if (prompt->history_searching) {
        prompt->history_searching = false;
        prompt->text.len = 0;
        if (prompt->history_counter < history->len) {
            cz::Str hist = history->get(prompt->history_counter);
            prompt->text.reserve(cz::heap_allocator(), hist.len);
            prompt->text.append(hist);
            prompt->cursor = prompt->text.len;
        }
    }
}

static cz::Vector<cz::Str>* prompt_history(Prompt_State* prompt, bool script) {
    return script ? &prompt->stdin_history : &prompt->history;
}

static void backward_word(cz::Str text, size_t* cursor) {
    while (*cursor > 0) {
        if (cz::is_alpha(text[*cursor - 1]))
            break;
        --*cursor;
    }
    while (*cursor > 0) {
        if (!cz::is_alpha(text[*cursor - 1]))
            break;
        --*cursor;
    }
}

static void forward_word(cz::Str text, size_t* cursor) {
    while (*cursor < text.len) {
        if (cz::is_alpha(text[*cursor]))
            break;
        ++*cursor;
    }
    while (*cursor < text.len) {
        if (!cz::is_alpha(text[*cursor]))
            break;
        ++*cursor;
    }
}

static bool handle_prompt_manipulation_commands(Shell_State* shell,
                                                Prompt_State* prompt,
                                                cz::Vector<Backlog_State*>* backlogs,
                                                Render_State* rend,
                                                uint16_t mod,
                                                SDL_Keycode key) {
    cz::Vector<cz::Str>* history = prompt_history(prompt, shell->active_process != -1);
    if ((mod & ~KMOD_SHIFT) == 0 && key == SDLK_BACKSPACE) {
        if (prompt->cursor > 0) {
            --prompt->cursor;
            prompt->text.remove(prompt->cursor);
        }
    } else if (mod == (KMOD_CTRL | KMOD_ALT) && key == SDLK_BACKSPACE) {
        prompt->text.remove_range(0, prompt->cursor);
        prompt->cursor = 0;
    } else if ((mod & ~KMOD_SHIFT) == 0 && key == SDLK_DELETE) {
        if (prompt->cursor < prompt->text.len) {
            prompt->text.remove(prompt->cursor);
        }
    } else if ((mod == KMOD_ALT && key == SDLK_DELETE) || (mod == KMOD_ALT && key == SDLK_d)) {
        size_t end = prompt->cursor;
        forward_word(prompt->text, &end);
        prompt->text.remove_range(prompt->cursor, end);
    } else if ((mod == KMOD_CTRL && key == SDLK_BACKSPACE) ||
               (mod == KMOD_ALT && key == SDLK_BACKSPACE)) {
        size_t end = prompt->cursor;
        backward_word(prompt->text, &prompt->cursor);
        prompt->text.remove_range(prompt->cursor, end);
    } else if (mod == KMOD_CTRL && key == SDLK_k) {
        prompt->text.remove_range(prompt->cursor, prompt->text.len);
    } else if (mod == (KMOD_CTRL | KMOD_ALT) && key == SDLK_BACKSPACE) {
        size_t end = prompt->cursor;
        prompt->cursor = 0;
        prompt->text.remove_range(prompt->cursor, end);
    } else if ((mod == 0 && key == SDLK_LEFT) || (mod == KMOD_CTRL && key == SDLK_b)) {
        if (prompt->cursor > 0) {
            --prompt->cursor;
        }
    } else if ((mod == 0 && key == SDLK_RIGHT) || (mod == KMOD_CTRL && key == SDLK_f)) {
        if (prompt->cursor < prompt->text.len) {
            ++prompt->cursor;
        }
    } else if ((mod == 0 && key == SDLK_UP) || (mod == KMOD_CTRL && key == SDLK_p)) {
        if (prompt->history_counter > 0) {
            --prompt->history_counter;
            prompt->text.len = 0;
            cz::Str hist = (*history)[prompt->history_counter];
            prompt->text.reserve(cz::heap_allocator(), hist.len);
            prompt->text.append(hist);
            prompt->cursor = prompt->text.len;
        }
    } else if ((mod == 0 && key == SDLK_DOWN) || (mod == KMOD_CTRL && key == SDLK_n)) {
        if (prompt->history_counter < history->len) {
            ++prompt->history_counter;
            prompt->text.len = 0;
            if (prompt->history_counter < history->len) {
                cz::Str hist = (*history)[prompt->history_counter];
                prompt->text.reserve(cz::heap_allocator(), hist.len);
                prompt->text.append(hist);
            }
            prompt->cursor = prompt->text.len;
        }
    } else if (mod == KMOD_CTRL && key == SDLK_r) {
        if (!prompt->history_searching) {
            prompt->history_searching = true;
            prompt->history_counter = history->len;
        }
        while (1) {
            if (prompt->history_counter == 0) {
                prompt->history_counter = history->len;
                break;
            }
            --prompt->history_counter;
            cz::Str hist = (*history)[prompt->history_counter];
            if (hist.contains_case_insensitive(prompt->text))
                break;
        }
    } else if (mod == KMOD_ALT && key == SDLK_r) {
        if (prompt->history_searching) {
            while (1) {
                ++prompt->history_counter;
                if (prompt->history_counter >= history->len) {
                    prompt->history_counter = history->len;
                    prompt->history_searching = false;
                    break;
                }
                cz::Str hist = (*history)[prompt->history_counter];
                if (hist.contains_case_insensitive(prompt->text))
                    break;
            }
        }
    } else if (mod == KMOD_CTRL && key == SDLK_g) {
        resolve_history_searching(prompt, history);
    } else if ((mod == 0 && key == SDLK_HOME) || (mod == KMOD_CTRL && key == SDLK_a)) {
        prompt->cursor = 0;
    } else if ((mod == 0 && key == SDLK_END) || (mod == KMOD_CTRL && key == SDLK_e)) {
        prompt->cursor = prompt->text.len;
    } else if ((mod == KMOD_CTRL && key == SDLK_LEFT) || (mod == KMOD_ALT && key == SDLK_LEFT) ||
               (mod == KMOD_ALT && key == SDLK_b)) {
        backward_word(prompt->text, &prompt->cursor);
    } else if ((mod == KMOD_CTRL && key == SDLK_RIGHT) || (mod == KMOD_ALT && key == SDLK_RIGHT) ||
               (mod == KMOD_ALT && key == SDLK_f)) {
        forward_word(prompt->text, &prompt->cursor);
    } else if (mod == KMOD_SHIFT && key == SDLK_INSERT) {
        char* clip = SDL_GetClipboardText();
        if (clip) {
            CZ_DEFER(SDL_free(clip));
            size_t len = strlen(clip);
            cz::String str = {clip, len, len};
            cz::strip_carriage_returns(&str);
            prompt->text.reserve(cz::heap_allocator(), str.len);
            prompt->text.insert(prompt->cursor, str);
            prompt->cursor += str.len;
        }
    } else {
        return false;
    }

    ensure_prompt_on_screen(rend, *backlogs);
    rend->auto_page = false;
    rend->auto_scroll = true;
    return true;
}

static bool handle_scroll_commands(Shell_State* shell,
                                   Prompt_State* prompt,
                                   cz::Slice<Backlog_State*> backlogs,
                                   Render_State* rend,
                                   uint16_t mod,
                                   SDL_Keycode key) {
    if ((mod == 0 && key == SDLK_PAGEDOWN) || (mod == KMOD_CTRL && key == SDLK_v)) {
        int lines = cz::max(rend->window_rows, 6) - 3;
        scroll_down(rend, backlogs, lines);
    } else if ((mod == 0 && key == SDLK_PAGEUP) || (mod == KMOD_ALT && key == SDLK_v)) {
        int lines = cz::max(rend->window_rows, 6) - 3;
        scroll_up(rend, backlogs, lines);
    } else if (mod == KMOD_ALT && key == SDLK_n) {
        scroll_down(rend, backlogs, 1);
    } else if (mod == KMOD_ALT && key == SDLK_p) {
        scroll_up(rend, backlogs, 1);
    } else if (mod == KMOD_ALT && key == SDLK_LESS) {
        rend->backlog_start = {};
        if (backlogs.len > 0)
            rend->backlog_start.outer = backlogs.len - 1;
    } else if (mod == (KMOD_CTRL | KMOD_ALT) && key == SDLK_b) {
        size_t outer = rend->backlog_start.outer;
        size_t inner = rend->backlog_start.inner;
        rend->backlog_start = {};
        if (inner > 0)
            rend->backlog_start.outer = outer;
        else if (outer > 0)
            rend->backlog_start.outer = outer - 1;
    } else if (mod == (KMOD_CTRL | KMOD_ALT) && key == SDLK_f) {
        size_t outer = rend->backlog_start.outer;
        size_t inner = rend->backlog_start.inner;
        rend->backlog_start = {};
        if (outer + 1 <= backlogs.len)
            outer++;
        rend->backlog_start.outer = outer;
    } else {
        return false;
    }

    shell->active_process = -1;
    rend->auto_page = false;
    rend->auto_scroll = false;
    rend->complete_redraw = true;
    return true;
}

static int process_events(cz::Vector<Backlog_State*>* backlogs,
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

            SDL_Keycode key = event.key.keysym.sym;
            if (key == SDLK_ESCAPE)
                return -1;

            if (handle_prompt_manipulation_commands(shell, prompt, backlogs, rend, mod, key)) {
                ++num_events;
                continue;
            }

            if (handle_scroll_commands(shell, prompt, *backlogs, rend, mod, key)) {
                ++num_events;
                continue;
            }

            if ((mod == KMOD_CTRL && event.key.keysym.sym == SDLK_c) ||
                event.key.keysym.sym == SDLK_RETURN) {
                rend->auto_page = false;
                rend->auto_scroll = true;

                {
                    cz::Vector<cz::Str>* history =
                        prompt_history(prompt, shell->active_process != -1);
                    resolve_history_searching(prompt, history);
                }

                Running_Script* script = active_process(shell);
                uint64_t process_id = (script ? script->id : prompt->process_id);
                Backlog_State* backlog;
                if (script) {
                    backlog = (*backlogs)[process_id];
                } else {
                    backlog = push_backlog(backlogs, process_id);
                    append_text(backlog, shell->working_directory);
                    append_text(backlog, " ");
                    append_text(backlog, prompt->prefix);
                }
                {
                    Backlog_Event event = {};
                    event.index = backlog->length;
                    event.type = BACKLOG_EVENT_START_INPUT;
                    backlog->events.reserve(cz::heap_allocator(), 1);
                    backlog->events.push(event);
                }
                append_text(backlog, prompt->text);
                {
                    Backlog_Event event = {};
                    event.index = backlog->length;
                    event.type = BACKLOG_EVENT_START_PROCESS;
                    backlog->events.reserve(cz::heap_allocator(), 1);
                    backlog->events.push(event);
                }
                append_text(backlog, "\n");

                if (event.key.keysym.sym == SDLK_RETURN) {
                    if (script) {
                        (void)script->in.write(cz::format(temp_allocator, prompt->text, '\n'));
                    } else {
                        rend->auto_page = cfg.on_spawn_auto_page;
                        rend->auto_scroll = cfg.on_spawn_auto_scroll;
                        if (!run_script(shell, backlog, prompt->text)) {
                            append_text(backlog, "Error: failed to execute\n");
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
                    cz::Vector<cz::Str>* history = prompt_history(prompt, script);
                    if (history->len == 0 || history->last() != prompt->text) {
                        history->reserve(cz::heap_allocator(), 1);
                        history->push(prompt->text.clone(prompt->history_arena.allocator()));
                    }
                }

                prompt->text.len = 0;
                prompt->cursor = 0;
                if (!script)
                    ++prompt->process_id;
                {
                    cz::Vector<cz::Str>* history =
                        prompt_history(prompt, shell->active_process != -1);
                    prompt->history_counter = history->len;
                }

                ensure_prompt_on_screen(rend, *backlogs);
                ++num_events;
            }

            if (mod == KMOD_CTRL && key == SDLK_z) {
                rend->auto_page = false;
                rend->auto_scroll = true;
                if (shell->active_process == -1) {
                    for (size_t i = shell->scripts.len; i-- > 0;) {
                        Running_Script* script = &shell->scripts[i];
                        if (script->in.is_open()) {
                            shell->active_process = script->id;
                            prompt->history_counter = prompt->stdin_history.len;
                            break;
                        }
                    }
                } else {
                    shell->active_process = -1;
                    prompt->history_counter = prompt->history.len;
                }
                ++num_events;
            }

            if (mod == KMOD_CTRL && key == SDLK_d) {
                if (shell->active_process == -1) {
                    // Should we exit Tesh?  I don't like when you're using 'less' and close
                    // your term accidentally.  But we're also not using 'less' so idk.
                } else {
                    Running_Script* script = active_process(shell);
                    script->in.close();
                    script->in = {};
                    shell->active_process = -1;
                    ++num_events;
                }
            }

            if (mod == KMOD_CTRL && key == SDLK_l) {
                rend->backlog_start = {};
                if (backlogs->len > 0) {
                    rend->backlog_start.outer = backlogs->len - 1;
                    Backlog_State* backlog = backlogs->last();
                    rend->backlog_start.inner = backlog->length;
                    if (backlog->length > 0 && backlog->get(backlog->length - 1) != '\n')
                        ++rend->backlog_start.inner;
                }
                rend->complete_redraw = true;
                ++num_events;
            }

            if (mod == KMOD_ALT && key == SDLK_GREATER) {
                rend->backlog_start = {};
                rend->backlog_start.outer = backlogs->len;
                rend->complete_redraw = true;
                ++num_events;

                rend->auto_page = false;
                rend->auto_scroll = true;
                int lines = cz::max(rend->window_rows, 3) - 3;
                scroll_up(rend, *backlogs, lines);
            }

            // Note: C-= used to zoom in so you don't have to hold shift.
            if (mod == KMOD_CTRL && (key == SDLK_EQUALS || key == SDLK_MINUS)) {
                int new_font_size = rend->font_size;
                if (key == SDLK_EQUALS) {
                    new_font_size += 4;
                } else {
                    new_font_size = cz::max(new_font_size - 4, 4);
                }
                resize_font(new_font_size, rend);
                ++num_events;
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
            ensure_prompt_on_screen(rend, *backlogs);
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

            const Uint8* state = SDL_GetKeyboardState(NULL);
            if (state[SDL_SCANCODE_LCTRL] || state[SDL_SCANCODE_RCTRL]) {
                int new_font_size = rend->font_size;
                if (event.wheel.y > 0) {
                    new_font_size += 2;
                } else {
                    new_font_size = cz::max(new_font_size - 2, 2);
                }
                resize_font(new_font_size, rend);
            } else {
                if (event.wheel.y < 0) {
                    scroll_down(rend, *backlogs, -event.wheel.y);
                } else if (event.wheel.y > 0) {
                    scroll_up(rend, *backlogs, event.wheel.y);
                }
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

static void load_history(Prompt_State* prompt, Shell_State* shell) {
    cz::Str home = {};
    if (!get_var(shell, "HOME", &home))
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
    if (!get_var(shell, "HOME", &home))
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
        {0x18, 0, 0, 0xff},    {0, 0x13, 0, 0xff},    {0, 0, 0x20, 0xff},
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
                if (key.len > 0) {
                    set_var(shell, key, value);
                    make_env_var(shell, key);
                }
            }
        }
    }

    // Set HOME to the user home directory.
    cz::Str temp;
    if (!get_var(shell, "HOME", &temp)) {
        cz::String home = {};
        if (cz::env::get_home(cz::heap_allocator(), &home)) {
            set_var(shell, "HOME", home);
            make_env_var(shell, "HOME");
        }
    }
#else
    extern char** environ;
    for (char** iter = environ; *iter; ++iter) {
        cz::Str line = *iter;
        cz::Str key, value;
        if (line.split_excluding('=', &key, &value)) {
            if (key.len > 0) {
                set_var(shell, key, value);
                make_env_var(shell, key);
            }
        }
    }
#endif
}

///////////////////////////////////////////////////////////////////////////////
// main
///////////////////////////////////////////////////////////////////////////////

int actual_main(int argc, char** argv) {
    Render_State rend = {};
    cz::Vector<Backlog_State*> backlogs = {};
    Prompt_State prompt = {};
    Shell_State shell = {};

    load_default_configuration();

    prompt.history_arena.init();
    prompt.process_id = 1;  // rc = 0

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
        SDL_CreateWindow("Tesh", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
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

    run_rc(&shell, push_backlog(&backlogs, 0));

    while (1) {
        uint32_t start_frame = SDL_GetTicks();

        temp_arena.clear();

        try {
            int status = process_events(&backlogs, &prompt, &rend, &shell);
            if (status < 0)
                break;

            bool force_quit = false;
            if (read_process_data(&shell, backlogs, &rend, &force_quit))
                status = 1;

            if (force_quit)
                break;

            if (rend.complete_redraw || status > 0 || shell.scripts.len > 0)
                render_frame(window, &rend, backlogs, &prompt, &shell);
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

void resize_font(int font_size, Render_State* rend) {
    TTF_Font* new_font = open_font(cfg.font_path, (int)(font_size * rend->dpi_scale));
    if (new_font) {
        close_font(rend);

        rend->font = new_font;
        rend->font_size = font_size;
        rend->font_height = TTF_FontLineSkip(rend->font);
        // TODO: handle failure
        TTF_GlyphMetrics(rend->font, ' ', nullptr, nullptr, nullptr, nullptr, &rend->font_width);

        rend->complete_redraw = true;
    }
}

static Backlog_State* push_backlog(cz::Vector<Backlog_State*>* backlogs, uint64_t id) {
    char* buffer = (char*)cz::heap_allocator().alloc({4096, 1});
    CZ_ASSERT(buffer);

    Backlog_State* backlog = permanent_allocator.alloc<Backlog_State>();
    CZ_ASSERT(backlog);
    *backlog = {};

    backlog->id = id;
    backlog->buffers.reserve(cz::heap_allocator(), 1);
    backlog->buffers.push(buffer);
    backlog->start = std::chrono::high_resolution_clock::now();

    backlogs->reserve(cz::heap_allocator(), 1);
    backlogs->push(backlog);

    return backlog;
}
