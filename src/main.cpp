#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <Tracy.hpp>
#include <cz/binary_search.hpp>
#include <cz/defer.hpp>
#include <cz/directory.hpp>
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
#include "solarized_dark.hpp"
#include "unicode.hpp"

static Backlog_State* push_backlog(cz::Vector<Backlog_State*>* backlogs, uint64_t id);
static float get_dpi_scale(SDL_Window* window);
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

    struct {
        bool is;
        size_t prefix_length;
        cz::Buffer_Array results_arena;
        cz::Vector<cz::Str> results;
        size_t current;
    } completion;
};

static cz::Vector<cz::Str>* prompt_history(Prompt_State* prompt, bool script);

///////////////////////////////////////////////////////////////////////////////
// Renderer methods
///////////////////////////////////////////////////////////////////////////////

static void make_info(cz::String* info,
                      Render_State* rend,
                      Shell_State* shell,
                      Backlog_State* backlog,
                      uint64_t first_line_index,
                      std::chrono::high_resolution_clock::time_point now) {
    if (backlog->cancelled)
        return;

    std::chrono::high_resolution_clock::time_point end = backlog->end;
    if (!backlog->done) {
        if (!lookup_process(shell, backlog->id)) {
            backlog->done = true;
            backlog->end = now;
        }
        end = now;
    }

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

static size_t make_backlog_code_point(char sequence[5], Backlog_State* backlog, size_t start) {
    size_t width = cz::min(unicode::utf8_width(sequence[0]), backlog->length - start);
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

static void render_string(SDL_Surface* window_surface,
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
        if (!render_code_point(window_surface, rend, info_start, background, foreground, seq,
                               set_tile)) {
            break;
        }
    }
}

static void render_info(SDL_Surface* window_surface,
                        Render_State* rend,
                        Visual_Point info_start,
                        Visual_Point info_end,
                        uint32_t background,
                        cz::Str info,
                        bool done) {
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

    uint8_t foreground = (done ? cfg.info_fg_color : cfg.info_running_fg_color);
    render_string(window_surface, rend, &info_start, background, foreground, info, false);
}

static uint64_t render_length(Backlog_State* backlog) {
    if (backlog->render_collapsed && backlog->lines.len > 0)
        return backlog->lines[0];
    return backlog->length;
}

static bool render_backlog(SDL_Surface* window_surface,
                           Render_State* rend,
                           Shell_State* shell,
                           std::chrono::high_resolution_clock::time_point now,
                           Backlog_State* backlog) {
    ZoneScoped;
    Visual_Point* point = &rend->backlog_end;
    uint64_t i = 0;
    if (point->outer == backlog->id) {
        i = point->inner;
    } else {
        point->outer++;
        point->inner = 0;
    }

    CZ_ASSERT(point->y >= 0);
    if (point->y >= rend->window_rows_ru)
        return false;

    uint64_t process_id = backlog->id;
    SDL_Color bg_color = cfg.process_colors[process_id % cfg.process_colors.len];
    if (shell->selected_process == process_id) {
        bg_color.r *= 2;
        bg_color.g *= 2;
        bg_color.b *= 2;
    }
    uint32_t background = SDL_MapRGB(window_surface->format, bg_color.r, bg_color.g, bg_color.b);

    cz::String info = {};
    make_info(&info, rend, shell, backlog, point->inner, now);
    bool info_has_start = false, info_has_end = false;
    Visual_Point info_start = {}, info_end = {};
    int info_y = point->y;
    int info_x_start = (int)(rend->window_cols - info.len);

    uint8_t fg_color = cfg.backlog_fg_color;

    size_t event_index = 0;

    uint64_t end = render_length(backlog);
    while (i < end) {
        while (event_index < backlog->events.len && backlog->events[event_index].index <= i) {
            Backlog_Event* event = &backlog->events[event_index];
            if (event->type == BACKLOG_EVENT_START_PROCESS) {
                fg_color = cfg.backlog_fg_color;
            } else if (event->type == BACKLOG_EVENT_START_INPUT) {
                fg_color = cfg.prompt_fg_color;
            } else if (event->type == BACKLOG_EVENT_START_DIRECTORY) {
                fg_color = cfg.info_fg_color;
            } else if (event->type == BACKLOG_EVENT_SET_GRAPHIC_RENDITION) {
                uint64_t gr = event->payload;
                fg_color = (uint8_t)((gr & GR_FOREGROUND_MASK) >> GR_FOREGROUND_SHIFT);
            } else {
                CZ_PANIC("unreachable");
            }
            ++event_index;
        }

        Visual_Point old_point = *point;

        // Get the chars that compose this code point.
        char seq[5] = {backlog->get(i)};
        i += make_backlog_code_point(seq, backlog, i);

        if (!render_code_point(window_surface, rend, point, background, fg_color, seq, true))
            break;

        if (!info_has_end && point->y != info_y) {
            info_has_end = true;
            info_end = old_point;
        }
        if (!info_has_start && (point->y != info_y || point->x > info_x_start - 1)) {
            info_has_start = true;
            info_start = old_point;
        }
    }

    rend->backlog_end.outer = backlog->id;
    rend->backlog_end.inner = i;

    if (rend->backlog_end.inner == backlog->length && backlog->length > 0 &&
        backlog->get(backlog->length - 1) != '\n') {
        Visual_Point old_point = *point;

        if (!render_code_point(window_surface, rend, point, background, cfg.prompt_fg_color, "\n",
                               true)) {
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

    if (info_has_start && info.len < rend->window_cols) {
        if (!info_has_end)
            info_end = info_start;
        info_start.x = info_x_start;
        render_info(window_surface, rend, info_start, info_end, background, info, backlog->done);
    }

    bg_color = {};
    background = SDL_MapRGB(window_surface->format, bg_color.r, bg_color.g, bg_color.b);
    if (!render_code_point(window_surface, rend, point, background, cfg.prompt_fg_color, "\n",
                           true)) {
        return false;
    }

    return true;
}

static void render_backlogs(SDL_Surface* window_surface,
                            Render_State* rend,
                            Shell_State* shell,
                            std::chrono::high_resolution_clock::time_point now,
                            cz::Slice<Backlog_State*> backlogs) {
    for (size_t i = rend->backlog_start.outer; i < backlogs.len; ++i) {
        if (!render_backlog(window_surface, rend, shell, now, backlogs[i]))
            break;
    }
}

static void render_prompt(SDL_Surface* window_surface,
                          Render_State* rend,
                          Prompt_State* prompt,
                          Shell_State* shell) {
    ZoneScoped;

    Visual_Point point = rend->backlog_end;
    point.outer++;
    point.inner = 0;

    uint64_t process_id =
        (shell->attached_process == -1 ? prompt->process_id : shell->attached_process);
    SDL_Color bg_color = cfg.process_colors[process_id % cfg.process_colors.len];
    if (shell->selected_process == -1 || shell->attached_process == shell->selected_process) {
        bg_color.r *= 2;
        bg_color.g *= 2;
        bg_color.b *= 2;
    }
    uint32_t background = SDL_MapRGB(window_surface->format, bg_color.r, bg_color.g, bg_color.b);

    if (shell->attached_process == -1) {
        render_string(window_surface, rend, &point, background, cfg.info_fg_color,
                      shell->working_directory, true);
        render_string(window_surface, rend, &point, background, cfg.backlog_fg_color,
                      prompt->prefix, true);
    } else {
        render_string(window_surface, rend, &point, background, cfg.backlog_fg_color, "> ", true);
    }

    bool drawn_cursor = false;
    SDL_Color prompt_fg_color = cfg.theme[cfg.prompt_fg_color];
    uint32_t cursor_color =
        SDL_MapRGB(window_surface->format, prompt_fg_color.r, prompt_fg_color.g, prompt_fg_color.b);
    for (size_t i = 0; i < prompt->text.len;) {
        bool draw_cursor = (!drawn_cursor && i >= prompt->cursor);
        SDL_Rect cursor_rect;
        if (draw_cursor) {
            cursor_rect = {point.x * rend->font_width - 1, point.y * rend->font_height, 2,
                           rend->font_height};
            SDL_FillRect(window_surface, &cursor_rect, cursor_color);
            drawn_cursor = true;
        }

        // Get the chars that compose this code point.
        char seq[5] = {prompt->text[i]};
        i += make_string_code_point(seq, prompt->text, i);

        // Render this code point.
        render_code_point(window_surface, rend, &point, background, cfg.prompt_fg_color, seq, true);

        // Draw cursor.
        if (draw_cursor && point.x != 0) {
            SDL_FillRect(window_surface, &cursor_rect, cursor_color);
        }
    }

    // Fill rest of line.
    Visual_Point eol = point;
    render_code_point(window_surface, rend, &point, background, cfg.backlog_fg_color, "\n", true);

    if (prompt->cursor == prompt->text.len) {
        // Draw cursor.
        SDL_Rect cursor_rect = {eol.x * rend->font_width - 1, eol.y * rend->font_height, 2,
                                rend->font_height};
        SDL_FillRect(window_surface, &cursor_rect, cursor_color);
    }

    if (prompt->history_searching) {
        cz::Str prefix = "HISTORY: ";
        render_string(window_surface, rend, &point, background, cfg.backlog_fg_color, prefix, true);
        cz::Vector<cz::Str>* history = prompt_history(prompt, shell->attached_process != -1);
        if (prompt->history_counter < history->len) {
            cz::Str hist = history->get(prompt->history_counter);
            render_string(window_surface, rend, &point, background, cfg.backlog_fg_color, hist,
                          true);
        }

        render_code_point(window_surface, rend, &point, background, cfg.backlog_fg_color, "\n",
                          true);
    }
}

static void ensure_prompt_on_screen(Render_State* rend, cz::Slice<Backlog_State*> backlogs);
static void ensure_end_of_selected_process_on_screen(Render_State* rend,
                                                     cz::Slice<Backlog_State*> backlogs,
                                                     uint64_t selected_process,
                                                     bool gotostart);

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

static void stop_selecting(Render_State* rend) {
    if (rend->selection.type != SELECT_DISABLED) {
        rend->selection.type = SELECT_DISABLED;
        rend->complete_redraw = true;
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

    if (rend->window_rows != shell->height || rend->window_cols != shell->width) {
        shell->height = rend->window_rows;
        shell->width = rend->window_cols;
        for (size_t i = 0; i < shell->scripts.len; ++i) {
            Running_Script* script = &shell->scripts[i];
            set_window_size(&script->tty, shell->width, shell->height);
        }
    }

    if (rend->auto_page)
        auto_scroll_start_paging(rend, backlogs);
    if (rend->auto_scroll)
        ensure_end_of_selected_process_on_screen(rend, backlogs, shell->selected_process, false);
    if (shell->attached_process != -1)
        ensure_prompt_on_screen(rend, backlogs);

    // TODO remove this
    rend->complete_redraw = true;

    if (rend->complete_redraw) {
        ZoneScopedN("draw_background");
        SDL_FillRect(window_surface, NULL, SDL_MapRGB(window_surface->format, 0x00, 0x00, 0x00));
        rend->backlog_end = rend->backlog_start;
    }

    if (!rend->grid_is_valid) {
        rend->grid.len = 0;
        size_t new_len = rend->window_rows_ru * rend->window_cols;
        rend->grid.reserve_exact(cz::heap_allocator(), new_len);
        rend->grid.len = new_len;
        rend->grid_is_valid = true;
    }
    memset(rend->grid.elems, 0, sizeof(Visual_Tile) * rend->grid.len);
    rend->selection.bg_color = SDL_MapRGB(window_surface->format, cfg.selection_bg_color.r,
                                          cfg.selection_bg_color.g, cfg.selection_bg_color.b);

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

    if (!next) {
        if (!background)
            backlog->exit_code = line->pipeline.last_exit_code;
        return false;
    }

    // TODO: we shouldn't throw away the arena and then immediately realloc it.
    // It's essentially free we're not even calling `free` but still.  Bad design.
    recycle_pipeline(shell, &line->pipeline);

    if (background) {
        script->bg.remove(line - script->bg.elems);
    } else {
        line->pipeline = {};
    }

    Error error = start_execute_line(shell, backlog, script, *next, background);
    if (error != Error_Success) {
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
    if (shell->attached_process == script->id)
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

#ifdef _WIN32
        cz::Input_File parent_out = script->tty.out;
#else
        cz::Input_File parent_out;
        parent_out.handle = script->tty.parent_bi;
#endif
        if (parent_out.is_open()) {
            int64_t result = 0;
            for (int rounds = 0; rounds < 1024; ++rounds) {
                // Even strip carriage returns on linux because
                // some programs (ex. 'git status') use CRLF.
                result = parent_out.read_strip_carriage_returns(buffer, sizeof(buffer),
                                                                &script->tty.out_carry);
                if (result <= 0)
                    break;

                // TODO: allow expanding max dynamically (don't close script->out here)
                result = append_text(backlog, {buffer, (size_t)result});
                if (result <= 0)
                    break;
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
        uint64_t end = render_length(backlog);
        while (1) {
            if (start->inner >= end) {
                if (start->inner == end && end > 0 && backlog->get(end - 1) != '\n') {
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
                end = render_length(backlog);

                if (start->y >= desired_y)
                    break;
                continue;
            }

            Visual_Point sp = *start;

            char seq[5] = {backlog->get(start->inner)};
            make_backlog_code_point(seq, backlog, start->inner);
            coord_trans(start, rend->window_cols, seq[0]);
            start->inner += strlen(seq) - 1;

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
    uint64_t* visual_line_starts = temp_allocator.alloc<uint64_t>(lines);

    Visual_Point* point = &rend->backlog_start;

    // If the prompt is at the top of the screen then reset the
    // point to the point where the prompt is in the last backlog.
    if (point->outer == backlogs.len) {
        if (point->outer == 0)
            return;
        point->outer--;
        Backlog_State* backlog = backlogs[point->outer];
        uint64_t end = render_length(backlog);
        point->inner = end + 1;
        if (end > 0 && backlog->get(end - 1) != '\n')
            point->inner++;
    }

    while (1) {
        Backlog_State* backlog = backlogs[point->outer];
        uint64_t end = render_length(backlog);
        uint64_t cursor = point->inner;
        if (cursor >= end + 1) {
            cursor--;
            lines--;
        }
        if (cursor >= end && end > 0 && backlog->get(end - 1) != '\n') {
            cursor--;
            lines--;
        }

        while (lines > 0 && cursor > 0) {
            size_t line_index;
            if (cz::binary_search(backlog->lines.as_slice(), cursor - 1, &line_index))
                ++line_index;  // Go after the match.
            uint64_t line_start = (line_index == 0 ? 0 : backlog->lines[line_index - 1]);

            size_t vlsi = 0;
            int visual_line_count = 0;
            {
                // First visual line start is at the physical line start.
                visual_line_starts[vlsi++] = line_start;
                if (vlsi == lines)
                    vlsi = 0;
                visual_line_count++;
            }

            uint64_t visual_column = 0;
            uint64_t actual_column = 0;
            for (uint64_t iter = line_start;;) {
                // Advance to next character.
                char seq[5] = {backlog->get(iter)};
                iter += make_backlog_code_point(seq, backlog, iter);
                if (iter >= cursor)
                    break;

                // Advance columns.
                uint64_t delta = 1;
                if (seq[0] == '\t') {
                    delta = cfg.tab_width - (actual_column % cfg.tab_width);
                }
                visual_column += delta;
                actual_column += delta;

                // If trip line boundary then record it.
                if (visual_column >= rend->window_cols) {
                    visual_column -= rend->window_cols;
                    visual_line_starts[vlsi++] = iter;
                    if (vlsi == lines)
                        vlsi = 0;
                    visual_line_count++;
                }
            }

            if (lines <= visual_line_count) {
                cursor = visual_line_starts[vlsi];
                lines = 0;
                break;
            }
            lines -= visual_line_count;

            if (line_start == 0)
                break;

            cursor = line_start;  // put cursor after the '\n'
        }

        if (lines == 0) {
            point->inner = cursor;
            // TODO set column
            break;
        }

        if (point->outer == 0)
            break;
        point->outer--;
        backlog = backlogs[point->outer];
        end = render_length(backlog);
        point->inner = end + 1;
        if (end > 0 && backlog->get(end - 1) != '\n')
            point->inner++;
    }

    point->y = 0;
    point->x = 0;
}

void clear_screen(Render_State* rend, Shell_State* shell, cz::Slice<Backlog_State*> backlogs) {
    rend->backlog_start = {};
    rend->backlog_start.outer = backlogs.len;
    if (shell->scripts.len > 0)
        scroll_up(rend, backlogs, 2);
    rend->complete_redraw = true;
    rend->auto_page = false;
    rend->auto_scroll = false;
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
        if (cz::is_alnum(text[*cursor - 1]))
            break;
        --*cursor;
    }
    while (*cursor > 0) {
        if (!cz::is_alnum(text[*cursor - 1]))
            break;
        --*cursor;
    }
}

static void forward_word(cz::Str text, size_t* cursor) {
    while (*cursor < text.len) {
        if (cz::is_alnum(text[*cursor]))
            break;
        ++*cursor;
    }
    while (*cursor < text.len) {
        if (!cz::is_alnum(text[*cursor]))
            break;
        ++*cursor;
    }
}

static void finish_prompt_manipulation(Shell_State* shell,
                                       Render_State* rend,
                                       cz::Slice<Backlog_State*> backlogs,
                                       Prompt_State* prompt,
                                       bool doing_completion) {
    shell->selected_process = shell->attached_process;
    ensure_prompt_on_screen(rend, backlogs);
    rend->auto_page = false;
    rend->auto_scroll = true;
    stop_selecting(rend);
    if (!doing_completion && prompt->completion.is) {
        prompt->completion.is = false;
        prompt->completion.prefix_length = 0;
        prompt->completion.results_arena.clear();
        prompt->completion.results.len = 0;
        prompt->completion.current = 0;
    }
}

static void run_paste(Prompt_State* prompt) {
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
}

static void start_completing(Prompt_State* prompt) {
    /////////////////////////////////////////////
    // Get the query
    /////////////////////////////////////////////

    size_t end = prompt->cursor;
    size_t start = end;
    while (start > 0) {
        // TODO break on semicolons and handle strings???
        if (cz::is_space(prompt->text[start - 1]))
            break;
        --start;
    }
    if (start == end)
        return;

    cz::Str query = prompt->text.slice(start, end);

    prompt->completion.is = true;

    /////////////////////////////////////////////
    // Split by last slash
    /////////////////////////////////////////////

    size_t slash = query.rfind_index('/');
#ifdef _WIN32
    size_t backslash = query.rfind_index('\\');
    if (slash > backslash)
        slash = backslash;
#endif

    cz::Str path = (slash == query.len ? "." : query.slice_end(slash));
    cz::Str prefix = (slash == query.len ? query : query.slice_start(slash + 1));
    prompt->completion.prefix_length = prefix.len;

    /////////////////////////////////////////////
    // Get all files matching the prefix.
    /////////////////////////////////////////////

    cz::Allocator path_allocator = prompt->completion.results_arena.allocator();

    // Put a dummy element at index 0 so we can tab through back to no completion as a form of undo.
    prompt->completion.results.reserve(cz::heap_allocator(), 1);
    prompt->completion.results.push(prefix.clone(path_allocator));

    cz::Directory_Iterator iterator;
    int result = iterator.init(path.clone_null_terminate(temp_allocator).buffer);
    if (result <= 0)
        return;

    while (1) {
        cz::Str name = iterator.str_name();
        if (name.starts_with(prefix)) {
            cz::String file = name.clone_null_terminate(path_allocator);
            prompt->completion.results.reserve(cz::heap_allocator(), 1);
            prompt->completion.results.push(file);
        }

        result = iterator.advance();
        if (result <= 0)
            break;
    }
    iterator.drop();
}

static bool handle_prompt_manipulation_commands(Shell_State* shell,
                                                Prompt_State* prompt,
                                                cz::Vector<Backlog_State*>* backlogs,
                                                Render_State* rend,
                                                uint16_t mod,
                                                SDL_Keycode key) {
    bool doing_completion = false;
    cz::Vector<cz::Str>* history = prompt_history(prompt, shell->attached_process != -1);
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
    } else if (mod == KMOD_ALT && key == SDLK_CARET) {
        const char* ptr = prompt->text.slice_start(prompt->cursor).find('\n');
        if (ptr)
            *(char*)ptr = ' ';
    } else if (mod == KMOD_CTRL && key == SDLK_t) {
        if (prompt->cursor < prompt->text.len && prompt->cursor > 0) {
            char ch1 = prompt->text[prompt->cursor - 1];
            char ch2 = prompt->text[prompt->cursor];
            prompt->text[prompt->cursor - 1] = ch2;
            prompt->text[prompt->cursor] = ch1;
            prompt->cursor++;
        }
    } else if ((mod & ~KMOD_SHIFT) == 0 && key == SDLK_TAB &&
               shell->selected_process == shell->attached_process) {
        doing_completion = true;

        if (prompt->completion.is) {
            // Delete previous completion.
            size_t prefix = prompt->completion.prefix_length;
            cz::Str prev = prompt->completion.results[prompt->completion.current];
            size_t del = prev.len - prefix;
            prompt->cursor -= del;
            prompt->text.remove_many(prompt->cursor, del);
        } else {
            start_completing(prompt);
            // Completion won't start if the user isn't at a thing that could conceivably be
            // completed.  Ex. at a blank prompt.  Allow other TAB bindings to take precedence.
            if (!prompt->completion.is)
                return false;
        }

        // Goto next / previous result.  Note: index 0 is a
        // dummy so this makes sense when we start completing.
        if (mod & KMOD_SHIFT) {
            if (prompt->completion.current == 0)
                prompt->completion.current = prompt->completion.results.len;
            prompt->completion.current--;
        } else {
            prompt->completion.current++;
            if (prompt->completion.current == prompt->completion.results.len)
                prompt->completion.current = 0;
        }

        // Insert the result.
        cz::Str curr = prompt->completion.results[prompt->completion.current];
        size_t prefix = prompt->completion.prefix_length;
        cz::Str ins = curr.slice_start(prefix);
        prompt->text.reserve(cz::heap_allocator(), ins.len);
        prompt->text.insert(prompt->cursor, ins);
        prompt->cursor += ins.len;
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
    } else if (mod == KMOD_CTRL && key == SDLK_a) {
        prompt->cursor = 0;
    } else if (mod == KMOD_CTRL && key == SDLK_e) {
        prompt->cursor = prompt->text.len;
    } else if ((mod == 0 && key == SDLK_HOME) || (mod == KMOD_ALT && key == SDLK_a)) {
        const char* nl = prompt->text.slice_end(prompt->cursor).rfind('\n');
        if (nl)
            prompt->cursor = nl + 1 - prompt->text.buffer;
        else
            prompt->cursor = 0;
    } else if ((mod == 0 && key == SDLK_END) || (mod == KMOD_ALT && key == SDLK_e)) {
        prompt->cursor += prompt->text.slice_start(prompt->cursor).find_index('\n');
    } else if ((mod == KMOD_CTRL && key == SDLK_LEFT) || (mod == KMOD_ALT && key == SDLK_LEFT) ||
               (mod == KMOD_ALT && key == SDLK_b)) {
        backward_word(prompt->text, &prompt->cursor);
    } else if ((mod == KMOD_CTRL && key == SDLK_RIGHT) || (mod == KMOD_ALT && key == SDLK_RIGHT) ||
               (mod == KMOD_ALT && key == SDLK_f)) {
        forward_word(prompt->text, &prompt->cursor);
    } else if ((mod == KMOD_SHIFT && key == SDLK_INSERT) ||
               (mod == (KMOD_CTRL | KMOD_SHIFT) && key == SDLK_v)) {
        run_paste(prompt);
    } else {
        return false;
    }

    finish_prompt_manipulation(shell, rend, *backlogs, prompt, doing_completion);
    return true;
}

static void ensure_selected_process_on_screen(Render_State* rend,
                                              cz::Slice<Backlog_State*> backlogs,
                                              uint64_t selected_process) {
    if (selected_process <= rend->backlog_start.outer) {
        rend->backlog_start = {};
        rend->backlog_start.outer = selected_process;
    } else {
        ensure_end_of_selected_process_on_screen(rend, backlogs, selected_process, true);
    }
}

static void scroll_to_end_of_selected_process(Render_State* rend,
                                              cz::Slice<Backlog_State*> backlogs,
                                              uint64_t selected_process) {
    rend->backlog_start = {};
    rend->backlog_start.outer = (selected_process == -1 ? backlogs.len : selected_process + 1);
    int lines = cz::max(rend->window_rows, 6) - 3;
    scroll_up(rend, backlogs, lines);
}

static void ensure_end_of_selected_process_on_screen(Render_State* rend,
                                                     cz::Slice<Backlog_State*> backlogs,
                                                     uint64_t selected_process,
                                                     bool gotostart) {
    // Go to the earliest point where the entire process / prompt is visible.
    Visual_Point backup = rend->backlog_start;
    scroll_to_end_of_selected_process(rend, backlogs, selected_process);

    if ((rend->backlog_start.outer > backup.outer) ||
        (rend->backlog_start.outer == backup.outer && rend->backlog_start.inner > backup.inner)) {
        if (gotostart && rend->backlog_start.outer ==
                             (selected_process == -1 ? backlogs.len : selected_process)) {
            // The process is really long so go to the start instead.
            rend->backlog_start.inner = 0;
        }
    } else {
        rend->backlog_start = backup;
    }

    // If offscreen then scroll up to fit it.
    if (selected_process < rend->backlog_start.outer) {
        rend->backlog_start = {};
        rend->backlog_start.outer = selected_process;
    }
}

static bool handle_scroll_commands(Shell_State* shell,
                                   Prompt_State* prompt,
                                   cz::Slice<Backlog_State*> backlogs,
                                   Render_State* rend,
                                   uint16_t mod,
                                   SDL_Keycode key) {
    bool auto_scroll = false;
    if ((mod == 0 && key == SDLK_PAGEDOWN) || (mod == KMOD_CTRL && key == SDLK_v)) {
        int lines = cz::max(rend->window_rows, 6) - 3;
        scroll_down(rend, backlogs, lines);
    } else if ((mod == 0 && key == SDLK_PAGEUP) || (mod == KMOD_ALT && key == SDLK_v)) {
        int lines = cz::max(rend->window_rows, 6) - 3;
        scroll_up(rend, backlogs, lines);
    } else if ((mod == KMOD_CTRL && key == SDLK_d) &&
               (shell->attached_process == -1 && prompt->text.len == 0)) {
        int lines = rend->window_rows / 2;
        scroll_down(rend, backlogs, lines);
    } else if (mod == KMOD_CTRL && key == SDLK_u) {
        int lines = rend->window_rows / 2;
        scroll_up(rend, backlogs, lines);
    } else if (mod == KMOD_ALT && key == SDLK_n) {
        scroll_down(rend, backlogs, 1);
    } else if (mod == KMOD_ALT && key == SDLK_p) {
        scroll_up(rend, backlogs, 1);
    } else if (mod == KMOD_ALT && key == SDLK_LESS) {
        // Goto start of selected process.
        rend->backlog_start = {};
        rend->backlog_start.outer =
            (shell->selected_process == -1 ? backlogs.len : shell->selected_process);
    } else if (mod == KMOD_ALT && key == SDLK_GREATER) {
        // Goto end of selected process.
        scroll_to_end_of_selected_process(rend, backlogs, shell->selected_process);
        auto_scroll = true;
    } else if (mod == (KMOD_CTRL | KMOD_ALT) && key == SDLK_b) {
        // Select the process before the currently selected
        // process, or the last process if this is the prompt.
        if (shell->selected_process == -1 && backlogs.len > 0) {
            shell->selected_process = backlogs.len - 1;
        } else if (shell->selected_process > 0) {
            --shell->selected_process;
        }
        ensure_selected_process_on_screen(rend, backlogs, shell->selected_process);
    } else if (mod == (KMOD_CTRL | KMOD_ALT) && key == SDLK_f) {
        // Select the next process, or the prompt if this is the last process.
        if (shell->selected_process != -1 && shell->selected_process + 1 < backlogs.len)
            ++shell->selected_process;
        else
            shell->selected_process = -1;
        ensure_selected_process_on_screen(rend, backlogs, shell->selected_process);
    } else if (mod == 0 && key == SDLK_TAB && shell->selected_process != -1) {
        Backlog_State* backlog = backlogs[shell->selected_process];
        backlog->render_collapsed = !backlog->render_collapsed;
        ensure_selected_process_on_screen(rend, backlogs, shell->selected_process);
        if (shell->attached_process == backlog->id)
            shell->attached_process = -1;
    } else {
        return false;
    }

    rend->auto_page = false;
    rend->auto_scroll = auto_scroll;
    rend->complete_redraw = true;
    return true;
}

static void push_backlog_event(Backlog_State* backlog, Backlog_Event_Type event_type) {
    Backlog_Event event = {};
    event.index = backlog->length;
    event.type = event_type;
    backlog->events.reserve(cz::heap_allocator(), 1);
    backlog->events.push(event);
}

static void append_piece(cz::String* clip,
                         size_t* off,
                         size_t inner_start,
                         size_t inner_end,
                         cz::Str part) {
    cz::Str piece = part;
    if (inner_end >= *off && inner_start < piece.len + *off) {
        if (inner_end + 1 < piece.len + *off)
            piece = piece.slice_end(inner_end - *off + 1);
        if (inner_start >= *off)
            piece = piece.slice_start(inner_start - *off);
        clip->append(piece);
    }
    *off += part.len;
}

static void set_clipboard_contents_to_selection(Render_State* rend,
                                                Shell_State* shell,
                                                Prompt_State* prompt,
                                                cz::Slice<Backlog_State*> backlogs) {
    cz::String clip = {};
    CZ_DEFER(clip.drop(temp_allocator));
    for (size_t outer = rend->selection.start.outer; outer <= rend->selection.end.outer; ++outer) {
        if (outer - 1 < backlogs.len) {
            Backlog_State* backlog = backlogs.get(outer - 1);

            size_t inner_start = 0;
            size_t inner_end = backlog->length;
            if (outer == rend->selection.start.outer)
                inner_start = rend->selection.start.inner;
            if (outer == rend->selection.end.outer)
                inner_end = rend->selection.end.inner;

            clip.reserve(temp_allocator, inner_end - inner_start + 2);

            // TODO: make this append a bucket at a time
            for (size_t inner = inner_start; inner < inner_end; ++inner) {
                clip.push(backlog->get(inner));
            }

            if (inner_end >= backlog->length) {
                clip.reserve(temp_allocator, inner_end + 1 - backlog->length + 1);
                for (size_t i = backlog->length; i <= inner_end; ++i)
                    clip.push('\n');
            } else
                clip.push(backlog->get(inner_end));
        } else {
            size_t inner_start = 0;
            size_t inner_end = shell->working_directory.len + prompt->prefix.len + prompt->text.len;
            if (outer == rend->selection.start.outer)
                inner_start = rend->selection.start.inner;
            if (outer == rend->selection.end.outer)
                inner_end = rend->selection.end.inner;

            clip.reserve(temp_allocator, inner_end - inner_start + 2);
            size_t off = 0;
            append_piece(&clip, &off, inner_start, inner_end, shell->working_directory);
            append_piece(&clip, &off, inner_start, inner_end, prompt->prefix);
            append_piece(&clip, &off, inner_start, inner_end, prompt->text);
        }
    }
    clip.null_terminate();
    (void)SDL_SetClipboardText(clip.buffer);
}

static int word_char_category(char ch) {
    if (cz::is_alnum(ch))
        return 1;
    else if (cz::is_space(ch))
        return 2;
    else
        return 3;
}

static void expand_selection(Selection* selection,
                             Shell_State* shell,
                             Prompt_State* prompt,
                             cz::Slice<Backlog_State*> backlogs) {
    CZ_DEBUG_ASSERT(selection->start.outer != 0);
    CZ_DEBUG_ASSERT(selection->end.outer != 0);

    if (!selection->expand_word && !selection->expand_line)
        return;

    cz::String prompt_buffer = {};
    CZ_DEFER(prompt_buffer.drop(temp_allocator));

    if (selection->start.outer - 1 == backlogs.len || selection->end.outer - 1 == backlogs.len) {
        if (shell->attached_process == -1) {
            cz::append(temp_allocator, &prompt_buffer, shell->working_directory, prompt->prefix);
        } else {
            cz::append(temp_allocator, &prompt_buffer, "> ");
        }
        cz::append(temp_allocator, &prompt_buffer, prompt->text);
    }

    if (selection->expand_word) {
        uint64_t* inner = &selection->start.inner;
        if (selection->start.outer - 1 < backlogs.len) {
            Backlog_State* backlog = backlogs[selection->start.outer - 1];
            if (*inner < backlog->length)
                ++*inner;

            // Skip until we find a different category.
            int category = -1;
            while (*inner > 0) {
                char ch = backlog->get(*inner - 1);
                if (ch == '\n') {
                    if (category == -1)
                        --*inner;
                    break;
                }
                int cat2 = word_char_category(ch);
                if (category == -1)
                    category = cat2;
                else if (category != cat2)
                    break;
                --*inner;
            }
        } else {
            // Don't expand if we're on a newline character.
            if (*inner < prompt_buffer.len) {
                ++*inner;
                // Skip until we find a different category.
                int category = -1;
                while (*inner > 0) {
                    char ch = prompt_buffer[*inner - 1];
                    if (ch == '\n') {
                        if (category == -1)
                            --*inner;
                        break;
                    }
                    int cat2 = word_char_category(ch);
                    if (category == -1)
                        category = cat2;
                    else if (category != cat2)
                        break;
                    --*inner;
                }
            }
        }

        inner = &selection->end.inner;
        if (selection->end.outer - 1 < backlogs.len) {
            Backlog_State* backlog = backlogs[selection->end.outer - 1];
            // Skip until we find a different category.
            int category = -1;
            while (*inner < backlog->length) {
                char ch = backlog->get(*inner);
                if (ch == '\n') {
                    if (category == -1)
                        ++*inner;
                    break;
                }
                int cat2 = word_char_category(ch);
                if (category == -1)
                    category = cat2;
                else if (category != cat2)
                    break;
                ++*inner;
            }
        } else {
            if (*inner >= prompt_buffer.len) {
                ++*inner;
            } else {
                // Skip until we find a different category.
                int category = -1;
                while (*inner < prompt_buffer.len) {
                    char ch = prompt_buffer[*inner];
                    if (ch == '\n') {
                        if (category == -1)
                            ++*inner;
                        break;
                    }
                    int cat2 = word_char_category(ch);
                    if (category == -1)
                        category = cat2;
                    else if (category != cat2)
                        break;
                    ++*inner;
                }
            }
        }
        if (*inner > 0)
            --*inner;
    } else if (selection->expand_line) {
        // Goto start of line.
        uint64_t* inner = &selection->start.inner;
        if (selection->start.outer - 1 < backlogs.len) {
            Backlog_State* backlog = backlogs[selection->start.outer - 1];
            while (*inner > 0) {
                if (backlog->get(*inner - 1) == '\n')
                    break;
                --*inner;
            }
        } else {
            while (*inner > 0) {
                if (prompt_buffer.get(*inner - 1) == '\n')
                    break;
                --*inner;
            }
        }

        // Goto start of next line.
        inner = &selection->end.inner;
        if (selection->end.outer - 1 < backlogs.len) {
            Backlog_State* backlog = backlogs[selection->end.outer - 1];
            while (*inner < backlog->length) {
                if (backlog->get(*inner) == '\n')
                    break;
                ++*inner;
            }
        } else {
            while (*inner < prompt_buffer.len) {
                if (prompt_buffer.get(*inner) == '\n')
                    break;
                ++*inner;
            }
        }
    }
}

static int process_events(cz::Vector<Backlog_State*>* backlogs,
                          Prompt_State* prompt,
                          Render_State* rend,
                          Shell_State* shell,
                          SDL_Window* window) {
    static uint32_t ignore_key_events_until = 0;

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
                event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                continue;
            }
            if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                // Ignore keyboard input events because, on Linux, if a window closes due to a
                // key combo (ex C-d), then we are selected, then all the depressed keys will
                // be sent to our process.  But the user didn't press them in our process.
                ignore_key_events_until = event.window.timestamp + 10;
            }
            if (event.window.event == SDL_WINDOWEVENT_MOVED ||
                event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                // Process dpi changes.
                float new_dpi_scale = get_dpi_scale(window);
                bool dpi_changed = (rend->dpi_scale + 0.01f < new_dpi_scale ||  //
                                    rend->dpi_scale - 0.01f > new_dpi_scale);
                if (dpi_changed) {
                    int w, h;
                    SDL_GetWindowSize(window, &w, &h);
                    w = (int)(w * (new_dpi_scale / rend->dpi_scale));
                    h = (int)(h * (new_dpi_scale / rend->dpi_scale));
                    SDL_SetWindowSize(window, w, h);
                    rend->dpi_scale = new_dpi_scale;
                    resize_font(rend->font_size, rend);
                }
            }

            rend->grid_is_valid = false;
            rend->complete_redraw = true;
            ++num_events;
            break;

        case SDL_KEYDOWN: {
            if (event.key.timestamp < ignore_key_events_until)
                break;

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
                        prompt_history(prompt, shell->attached_process != -1);
                    resolve_history_searching(prompt, history);
                }

                Running_Script* script = attached_process(shell);
                uint64_t process_id = (script ? script->id : prompt->process_id);
                Backlog_State* backlog;
                if (script) {
                    backlog = (*backlogs)[process_id];
                } else {
                    backlog = push_backlog(backlogs, process_id);
                    push_backlog_event(backlog, BACKLOG_EVENT_START_DIRECTORY);
                    append_text(backlog, shell->working_directory);
                    push_backlog_event(backlog, BACKLOG_EVENT_START_PROCESS);
                    append_text(backlog, prompt->prefix);
                }
                push_backlog_event(backlog, BACKLOG_EVENT_START_INPUT);
                append_text(backlog, prompt->text);
                push_backlog_event(backlog, BACKLOG_EVENT_START_PROCESS);
                append_text(backlog, "\n");

                if (event.key.keysym.sym == SDLK_RETURN) {
                    if (script) {
                        cz::Str message = cz::format(temp_allocator, prompt->text, '\n');
                        (void)tty_write(&script->tty, message);
                    } else {
                        rend->auto_page = cfg.on_spawn_auto_page;
                        rend->auto_scroll = cfg.on_spawn_auto_scroll;
                        if (!run_script(shell, backlog, prompt->text)) {
                            append_text(backlog, "Error: failed to execute\n");
                        }
                        shell->selected_process = backlog->id;
                    }
                } else {
                    if (script) {
#ifdef TRACY_ENABLE
                        cz::String message =
                            cz::format(temp_allocator, "End: ", script->fg.pipeline.command_line);
                        TracyMessage(message.buffer, message.len);
#endif

                        backlog->exit_code = -1;
                        recycle_process(shell, script);
                    } else {
                        backlog->done = true;
                        backlog->cancelled = true;
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
                        prompt_history(prompt, shell->attached_process != -1);
                    prompt->history_counter = history->len;
                }

                ensure_prompt_on_screen(rend, *backlogs);
                ++num_events;
            }

            if (mod == KMOD_CTRL && key == SDLK_z) {
                rend->auto_page = false;
                rend->auto_scroll = true;
                if (shell->attached_process == -1) {
                    // If the selected process is still running then attach to it.
                    // Otherwise, attach to the most recently launched process.
                    if (selected_process(shell)) {
                        shell->attached_process = shell->selected_process;
                    } else {
                        for (size_t i = shell->scripts.len; i-- > 0;) {
                            Running_Script* script = &shell->scripts[i];
                            shell->attached_process = script->id;
                            shell->selected_process = shell->attached_process;
                            prompt->history_counter = prompt->stdin_history.len;
                            break;
                        }
                    }
                } else {
                    shell->attached_process = -1;
                    shell->selected_process = shell->attached_process;
                    prompt->history_counter = prompt->history.len;
                }
                ++num_events;
            }

            if (mod == KMOD_CTRL && key == SDLK_d) {
                if (prompt->cursor < prompt->text.len) {
                    prompt->text.remove(prompt->cursor);
                    ++num_events;
                } else if (shell->attached_process != -1) {
                    Running_Script* script = attached_process(shell);

                    // 4 = EOT / EOF
                    (void)tty_write(&script->tty, "\4");

                    shell->attached_process = -1;
                    shell->selected_process = shell->attached_process;
                    ++num_events;
                }
            }

            if (mod == KMOD_CTRL && key == SDLK_l) {
                clear_screen(rend, shell, *backlogs);
                ++num_events;
            }

            if (mod == KMOD_ALT && key == SDLK_GREATER) {
                rend->backlog_start = {};
                rend->backlog_start.outer = backlogs->len;
                rend->complete_redraw = true;
                ++num_events;

                shell->selected_process = shell->attached_process;
                rend->auto_page = false;
                rend->auto_scroll = true;
                int lines = cz::max(rend->window_rows, 3) - 3;
                scroll_up(rend, *backlogs, lines);
            }

            if ((mod == KMOD_CTRL && key == SDLK_INSERT) ||
                (mod == (KMOD_CTRL | KMOD_SHIFT) && key == SDLK_c)) {
                // Copy selected region.
                if (rend->selection.type == SELECT_REGION ||
                    rend->selection.type == SELECT_FINISHED) {
                    rend->selection.type = SELECT_DISABLED;
                    rend->complete_redraw = true;
                    ++num_events;
                    set_clipboard_contents_to_selection(rend, shell, prompt, *backlogs);
                }
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
            if (event.key.timestamp < ignore_key_events_until)
                break;

            // SDL will send a text input and a key down event even when modifier keys
            // are held down.  But we only want to handle unmodified text input here.
            SDL_Keymod mods = SDL_GetModState();
            if (mods & (KMOD_CTRL | KMOD_ALT))
                break;

            cz::Str text = event.text.text;
            prompt->text.reserve(cz::heap_allocator(), text.len);
            prompt->text.insert(prompt->cursor, text);
            prompt->cursor += text.len;
            finish_prompt_manipulation(shell, rend, *backlogs, prompt, false);
            ++num_events;
        } break;

        case SDL_MOUSEWHEEL: {
            rend->auto_page = false;
            rend->auto_scroll = false;
            shell->attached_process = -1;

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
                } else if (event.wheel.y < 0) {
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

        case SDL_MOUSEBUTTONDOWN: {
            if (event.button.button == SDL_BUTTON_LEFT) {
                shell->selected_process = -1;
                rend->auto_page = false;
                rend->auto_scroll = false;
                rend->selection.type = SELECT_DISABLED;

                if (!rend->grid_is_valid)
                    break;

                int row = event.button.y / rend->font_height;
                int column = event.button.x / rend->font_width;
                Visual_Tile tile = rend->grid[row * rend->window_cols + column];

                if (tile.outer == 0)
                    break;

                rend->selection.expand_word = 0;
                rend->selection.expand_line = 0;

                if (event.button.clicks % 3 == 0) {
                    rend->selection.type = SELECT_REGION;
                    rend->selection.expand_line = 1;
                } else if (event.button.clicks % 3 == 2) {
                    rend->selection.type = SELECT_REGION;
                    rend->selection.expand_word = 1;
                } else {
                    rend->selection.type = SELECT_EMPTY;
                }

                shell->selected_process = tile.outer - 1;
                if (shell->selected_process == backlogs->len)
                    shell->selected_process = -1;

                rend->selection.down = tile;
                rend->selection.current = tile;
                rend->selection.start = tile;
                rend->selection.end = tile;

                expand_selection(&rend->selection, shell, prompt, *backlogs);

                rend->complete_redraw = true;
                ++num_events;
            } else if (event.button.button == SDL_BUTTON_MIDDLE) {
                run_paste(prompt);
                finish_prompt_manipulation(shell, rend, *backlogs, prompt, false);
                ++num_events;
            }
        } break;

        case SDL_MOUSEBUTTONUP: {
            if (event.button.button == SDL_BUTTON_LEFT) {
                if (rend->selection.type == SELECT_REGION) {
                    rend->selection.type = SELECT_FINISHED;
                    if (cfg.on_select_auto_copy) {
                        set_clipboard_contents_to_selection(rend, shell, prompt, *backlogs);
                    }
                } else {
                    rend->selection.type = SELECT_DISABLED;
                }
                rend->complete_redraw = true;
                ++num_events;
            }
        } break;

        case SDL_MOUSEMOTION: {
            if (!rend->grid_is_valid)
                break;

            if (rend->selection.type == SELECT_DISABLED || rend->selection.type == SELECT_FINISHED)
                break;

            int row = event.motion.y / rend->font_height;
            int column = event.motion.x / rend->font_width;
            Visual_Tile tile = rend->grid[row * rend->window_cols + column];

            if (tile.outer == 0) {
                tile.outer = backlogs->len + 1;
                tile.inner = shell->working_directory.len + prompt->prefix.len + prompt->text.len;
            }

            rend->selection.type = SELECT_REGION;
            rend->selection.current = tile;

            if ((rend->selection.current.outer < rend->selection.down.outer) ||
                (rend->selection.current.outer == rend->selection.down.outer &&
                 rend->selection.current.inner < rend->selection.down.inner)) {
                rend->selection.start = rend->selection.current;
                rend->selection.end = rend->selection.down;
            } else {
                rend->selection.start = rend->selection.down;
                rend->selection.end = rend->selection.current;
            }

            expand_selection(&rend->selection, shell, prompt, *backlogs);

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

    cz::Carriage_Return_Carry carry = {};
    cz::String element = {};
    while (1) {
        int64_t result = file.read_text(buffer.buffer, buffer.cap, &carry);
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

    cfg.on_select_auto_copy = true;

#ifdef _WIN32
    cfg.font_path = "C:/Windows/Fonts/MesloLGM-Regular.ttf";
#else
    cfg.font_path = "/usr/share/fonts/TTF/MesloLGMDZ-Regular.ttf";
#endif
    cfg.default_font_size = 12;
    cfg.tab_width = 8;

#ifdef _WIN32
    // Windows just doesn't have functionality we don't implement.
    cfg.builtin_level = 2;
#else
    // Linux we can fall back to default implementations that better.
    cfg.builtin_level = 1;
#endif

    cfg.max_length = ((uint64_t)1 << 30);  // 1GB

    static SDL_Color process_colors[] = {
        {0x18, 0, 0, 0xff},    {0, 0x13, 0, 0xff},    {0, 0, 0x20, 0xff},
        {0x11, 0x11, 0, 0xff}, {0, 0x11, 0x11, 0xff}, {0x11, 0, 0x17, 0xff},
    };
    cfg.process_colors = process_colors;

    cfg.theme = solarized_dark;

    cfg.backlog_fg_color = 7;
    cfg.prompt_fg_color = 51;
    cfg.info_fg_color = 201;
    cfg.info_running_fg_color = 154;
    cfg.selection_fg_color = 7;
    cfg.selection_bg_color = {0x66, 0x00, 0x66, 0xff};
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
    prompt.completion.results_arena.init();
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

    prompt.prefix = " $ ";
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
    SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
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

    rend.dpi_scale = get_dpi_scale(NULL);

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

    {
        int w, h;
        SDL_GetWindowSize(window, &w, &h);
        shell.width = w / rend.font_width;
        shell.height = h / rend.font_height;
    }

    run_rc(&shell, push_backlog(&backlogs, 0));

    while (1) {
        uint32_t start_frame = SDL_GetTicks();

        temp_arena.clear();

        try {
            int status = process_events(&backlogs, &prompt, &rend, &shell, window);
            if (status < 0)
                break;

            bool force_quit = false;
            if (read_process_data(&shell, backlogs, &rend, &force_quit))
                status = 1;

            if (force_quit)
                break;

            if (rend.complete_redraw || status > 0 || shell.scripts.len > 0 || !rend.grid_is_valid)
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
    backlog->max_length = cfg.max_length;
    backlog->buffers.reserve(cz::heap_allocator(), 1);
    backlog->buffers.push(buffer);
    backlog->start = std::chrono::high_resolution_clock::now();
    backlog->graphics_rendition = (7 << GR_FOREGROUND_SHIFT);

    backlogs->reserve(cz::heap_allocator(), 1);
    backlogs->push(backlog);

    return backlog;
}

static float get_dpi_scale(SDL_Window* window) {
    int display = SDL_GetWindowDisplayIndex(window);
    if (display == -1)
        display = 0;

    const float dpi_default = 96.0f;
    float dpi = 0;
    if (SDL_GetDisplayDPI(display, &dpi, NULL, NULL) != 0)
        return 1.0f;  // failure so assume no scaling
    return dpi / dpi_default;
}
