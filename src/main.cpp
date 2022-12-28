#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <Tracy.hpp>
#include <cz/binary_search.hpp>
#include <cz/date.hpp>
#include <cz/dedup.hpp>
#include <cz/defer.hpp>
#include <cz/directory.hpp>
#include <cz/env.hpp>
#include <cz/format.hpp>
#include <cz/heap.hpp>
#include <cz/path.hpp>
#include <cz/process.hpp>
#include <cz/sort.hpp>
#include <cz/string.hpp>
#include <cz/utf.hpp>
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
#include "prompt.hpp"
#include "render.hpp"
#include "search.hpp"
#include "shell.hpp"
#include "solarized_dark.hpp"
#include "unicode.hpp"

#include "UbuntuMono.h"

///////////////////////////////////////////////////////////////////////////////
// Forward declarations
///////////////////////////////////////////////////////////////////////////////

static Backlog_State* push_backlog(cz::Vector<Backlog_State*>* backlogs, uint64_t id);
static float get_dpi_scale(SDL_Window* window);
static void scroll_down1(Render_State* rend, int lines);
static void scroll_down(Render_State* rend, int lines);
static cz::Vector<cz::Str>* prompt_history(Prompt_State* prompt, bool script);
static void stop_merging_edits(Prompt_State* prompt);
static void stop_completing(Prompt_State* prompt);
static int word_char_category(char ch);
static void finish_hyperlink(Backlog_State* backlog);
static void load_cursors(Render_State* rend);
static void set_cursor(Render_State* rend, Visual_Tile tile);
static const char* get_hyperlink_at(Render_State* rend, Visual_Tile tile);
static void render_prompt(SDL_Surface* window_surface,
                          Render_State* rend,
                          Prompt_State* prompt,
                          Search_State* search,
                          cz::Slice<Backlog_State*> backlogs,
                          Shell_State* shell);
static void kill_process(Shell_State* shell,
                         Render_State* rend,
                         Prompt_State* prompt,
                         cz::Slice<Backlog_State*> backlogs,
                         Backlog_State* backlog,
                         Running_Script* script);
void escape_arg(cz::Str arg, cz::String* script, cz::Allocator allocator, size_t extra);
void create_null_file();

///////////////////////////////////////////////////////////////////////////////
// Renderer methods
///////////////////////////////////////////////////////////////////////////////

static void make_info(cz::String* info,
                      Render_State* rend,
                      Shell_State* shell,
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
        if (!render_code_point(window_surface, rend, info_start, background, foreground, false, seq,
                               set_tile)) {
            return false;
        }
    }
    return true;
}

static void render_info(SDL_Surface* window_surface,
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
    if (point->y >= rend->window_rows_ru)
        return false;

    SDL_Color bg_color = cfg.process_colors[backlog->id % cfg.process_colors.len];
    if (rend->selected_outer == visindex) {
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
        if (!render_code_point(window_surface, rend, point, background, fg_color, underline, seq,
                               true)) {
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
        render_prompt(window_surface, rend, prompt, /*search=*/nullptr, backlogs, shell);
    } else if (rend->backlog_end.inner == backlog->length && backlog->length > 0 &&
               backlog->get(backlog->length - 1) != '\n') {
        Visual_Point old_point = *point;

        if (!render_code_point(window_surface, rend, point, background, cfg.prompt_fg_color, false,
                               "\n", true)) {
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
        render_info(window_surface, rend, info_start, info_end, background, info, backlog);
    }

    bg_color = {};
    background = SDL_MapRGB(window_surface->format, bg_color.r, bg_color.g, bg_color.b);
    if (!render_code_point(window_surface, rend, point, background, cfg.prompt_fg_color, false,
                           "\n", true)) {
        return false;
    }

    return true;
}

static void render_prompt(SDL_Surface* window_surface,
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
        point->y = rend->window_rows - 1;
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
        render_string(window_surface, rend, point, background, cfg.directory_fg_color,
                      prompt->prefix, true);
    } else if (rend->attached_outer == -1) {
        render_string(window_surface, rend, point, background, cfg.directory_fg_color,
                      get_wd(&shell->local), true);
        render_string(window_surface, rend, point, background, cfg.backlog_fg_color, prompt->prefix,
                      true);
    }

    bool drawn_cursor = false;
    SDL_Color prompt_fg_color = cfg.theme[cfg.prompt_fg_color];
    uint32_t cursor_color =
        SDL_MapRGB(window_surface->format, prompt_fg_color.r, prompt_fg_color.g, prompt_fg_color.b);
    for (size_t i = 0; i < prompt->text.len;) {
        bool draw_cursor = (!drawn_cursor && i >= prompt->cursor);
        SDL_Rect cursor_rect;
        if (draw_cursor) {
            cursor_rect = {point->x * rend->font_width - 1, point->y * rend->font_height, 2,
                           rend->font_height};
            SDL_FillRect(window_surface, &cursor_rect, cursor_color);
            drawn_cursor = true;
        }

        // Get the chars that compose this code point.
        char seq[5] = {prompt->text[i]};
        i += make_string_code_point(seq, prompt->text, i);

        // Render this code point.
        render_code_point(window_surface, rend, point, background, cfg.prompt_fg_color, false, seq,
                          true);

        // Draw cursor.
        if (draw_cursor && point->x != 0) {
            SDL_FillRect(window_surface, &cursor_rect, cursor_color);
        }
    }

    // Fill rest of line.
    Visual_Point eol = *point;
    render_code_point(window_surface, rend, point, background, cfg.backlog_fg_color, false, "\n",
                      true);

    if (prompt->cursor == prompt->text.len) {
        // Draw cursor.
        SDL_Rect cursor_rect = {eol.x * rend->font_width - 1, eol.y * rend->font_height, 2,
                                rend->font_height};
        SDL_FillRect(window_surface, &cursor_rect, cursor_color);
    }

    if (prompt->history_searching) {
        cz::Str prefix = "History:\n";
        render_string(window_surface, rend, point, background, cfg.backlog_fg_color, prefix, true);

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
                if (!render_string(window_surface, rend, point, background, color, hist, true)) {
                    break;
                }
                if (!render_code_point(window_surface, rend, point, background,
                                       cfg.backlog_fg_color, false, "\n", true)) {
                    break;
                }
            }
        }

        render_code_point(window_surface, rend, point, background, cfg.backlog_fg_color, false,
                          "\n", true);
    }

    if (prompt->completion.is) {
        cz::Str prefix = "Completions:\n";
        render_string(window_surface, rend, point, background, cfg.backlog_fg_color, prefix, true);
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
            render_string(window_surface, rend, point, background, color, result, true);

            for (size_t padding = result.len; padding < longest_entry + 1; padding++) {
                render_code_point(window_surface, rend, point, background, cfg.backlog_fg_color,
                                  false, " ", true);
            }

            chars_on_line += longest_entry + 1;
            if (chars_on_line + longest_entry + 1 > rend->window_cols) {
                render_code_point(window_surface, rend, point, background, cfg.backlog_fg_color,
                                  false, "\n", true);
                chars_on_line = 0;
            }
        }
        if (chars_on_line != 0) {
            render_code_point(window_surface, rend, point, background, cfg.backlog_fg_color, false,
                              "\n", true);
        }
    }
}

static void ensure_prompt_on_screen(Render_State* rend);
static void ensure_end_of_selected_process_on_screen(Render_State* rend,
                                                     uint64_t selected_outer,
                                                     bool gotostart);

static void auto_scroll_start_paging(Render_State* rend) {
    if (rend->window_rows <= 3)
        return;

    Visual_Point backup = rend->backlog_start;

    // If we put the previous prompt at the top what happens.
    Visual_Point top_prompt = {};
    top_prompt = {};
    top_prompt.outer = (rend->visbacklogs.len == 0 ? 0 : rend->visbacklogs.len - 1);

    rend->backlog_start = top_prompt;
    scroll_down1(rend, rend->window_rows - 3);

    if (rend->backlog_start.y + 3 >= rend->window_rows) {
        // More than one page of content so stop auto paging.
        rend->backlog_start = top_prompt;
        rend->complete_redraw = true;
        rend->scroll_mode = PROMPT_SCROLL;
    } else {
        rend->backlog_start = backup;
        ensure_prompt_on_screen(rend);
    }
}

static void stop_selecting(Render_State* rend) {
    if (rend->selection.type == SELECT_DISABLED)
        return;

    rend->selection.type = SELECT_DISABLED;
    rend->complete_redraw = true;
}

static void render_frame(SDL_Window* window,
                         Render_State* rend,
                         Prompt_State* prompt,
                         Search_State* search,
                         cz::Slice<Backlog_State*> backlogs,
                         Shell_State* shell) {
    ZoneScoped;

    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

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

    if (rend->scroll_mode == AUTO_PAGE)
        auto_scroll_start_paging(rend);
    if (rend->scroll_mode == AUTO_SCROLL)
        ensure_end_of_selected_process_on_screen(rend, rend->selected_outer, false);
    if (rend->attached_outer != -1)
        ensure_prompt_on_screen(rend);

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

    for (size_t i = rend->backlog_start.outer; i < rend->visbacklogs.len; ++i) {
        if (!render_backlog(window_surface, rend, shell, prompt, backlogs, now,
                            rend->visbacklogs[i], i)) {
            break;
        }
    }

    if (rend->attached_outer == -1)
        render_prompt(window_surface, rend, prompt, nullptr, backlogs, shell);

    if (search->is_searching)
        render_prompt(window_surface, rend, prompt, search, backlogs, shell);

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

static bool run_script(Shell_State* shell,
                       Backlog_State* backlog,
                       cz::Buffer_Array arena,
                       cz::Str text) {
#ifdef TRACY_ENABLE
    {
        cz::String message = cz::format(temp_allocator, "Start: ", text);
        TracyMessage(message.buffer, message.len);
    }
#endif

    // Root has to be kept alive for path traversal to work.
    Shell_Node* root = arena.allocator().alloc<Shell_Node>();
    *root = {};

    Error error = parse_script(arena.allocator(), root, text);
    if (error != Error_Success)
        goto fail;

    error = start_execute_script(shell, backlog, arena, root);
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

    append_text(backlog, "tesh: Error: ");
    append_text(backlog, error_string(error));
    append_text(backlog, "\n");
    backlog->exit_code = -1;
    backlog->done = true;
    backlog->end = std::chrono::steady_clock::now();
    // Decrement refcount in caller.

    recycle_arena(shell, arena);
    return false;
}

static bool read_process_data(Shell_State* shell,
                              cz::Slice<Backlog_State*> backlogs,
                              Render_State* rend,
                              Prompt_State* prompt,
                              bool* force_quit) {
    bool changes = false;
    for (size_t i = 0; i < shell->scripts.len; ++i) {
        Running_Script* script = &shell->scripts[i];
        Backlog_State* backlog = backlogs[script->id];
        size_t starting_length = backlog->length;

        if (tick_running_node(shell, rend, prompt, &script->root, &script->tty, backlog,
                              force_quit)) {
            if (*force_quit)
                return true;
            --i;  // TODO rate limit
        }

        if (script->root.fg_finished && script->root.bg.len == 0) {
            if (!backlog->done) {
                backlog->done = true;
                backlog->end = std::chrono::steady_clock::now();
                // Decrement refcount when read finishes below.

                // If we're attached then we auto scroll but we can hit an edge case where the
                // final output isn't scrolled to.  So we stop halfway through the output.  I
                // think it would be better if this just called `ensure_prompt_on_screen`.
                if (rend->attached_outer != -1 &&
                    rend->visbacklogs[rend->attached_outer]->id == script->id)  //
                {
                    rend->scroll_mode = AUTO_SCROLL;
                    rend->attached_outer = -1;
                    prompt->history_counter = prompt->history.len;
                }
            }

            // Wait for one second after the process ends so the pipes flush.
            using namespace std::chrono;
            steady_clock::duration elapsed = (steady_clock::now() - backlog->end);
            if (duration_cast<milliseconds>(elapsed).count() >= 1000) {
                recycle_process(shell, script);
                finish_hyperlink(backlog);
                backlog_dec_refcount(backlogs, backlog);

                changes = true;
                --i;
            }
        }

        if (backlog->length != starting_length)
            changes = true;
    }
    return changes;
}

///////////////////////////////////////////////////////////////////////////////
// User events
///////////////////////////////////////////////////////////////////////////////

static void scroll_down1(Render_State* rend, int lines) {
    Visual_Point* start = &rend->backlog_start;
    if (start->outer < rend->visbacklogs.len) {
        int desired_y = start->y + lines;
        Backlog_State* backlog = rend->visbacklogs[start->outer];
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
                if (start->outer == rend->visbacklogs.len)
                    break;
                backlog = rend->visbacklogs[start->outer];
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
static void scroll_down(Render_State* rend, int lines) {
    scroll_down1(rend, lines);
    rend->backlog_start.y = 0;
}

static void scroll_up(Render_State* rend, int lines) {
    uint64_t* visual_line_starts = temp_allocator.alloc<uint64_t>(lines);

    Visual_Point* point = &rend->backlog_start;

    // If the prompt is at the top of the screen then reset the
    // point to the point where the prompt is in the last backlog.
    if (point->outer == rend->visbacklogs.len) {
        if (point->outer == 0)
            return;
        point->outer--;
        Backlog_State* backlog = rend->visbacklogs[point->outer];
        uint64_t end = render_length(backlog);
        point->inner = end + 1;
        if (end > 0 && backlog->get(end - 1) != '\n')
            point->inner++;
    }

    while (1) {
        Backlog_State* backlog = rend->visbacklogs[point->outer];
        uint64_t end = render_length(backlog);
        uint64_t cursor = point->inner;

        // Deal with fake newline and spacer newline.
        if (lines > 0 && cursor >= end && end > 0) {
            if (cursor > 0)
                --cursor;
            while (lines > 0 && cursor >= end && end > 0) {
                cursor--;
                lines--;
            }
            cursor++;

            // Fake newlines get double counted above so undo that.
            if (cursor == end && end > 0 && backlog->get(end - 1) != '\n')
                lines++;
        }

        // Deal with actual buffer contents.
        while (lines > 0 && cursor > 0 && end > 0) {
            // Find start of physical line.
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

            // The following algorithm has an off by one error
            // if we start in the middle of a physical line.
            if (cursor > line_start)
                --cursor;

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
        backlog = rend->visbacklogs[point->outer];
        end = render_length(backlog);
        point->inner = end + 1;
        if (end > 0 && backlog->get(end - 1) != '\n')
            point->inner++;
    }

    point->y = 0;
    point->x = 0;
}

void clear_screen(Render_State* rend, Shell_State* shell, Prompt_State* prompt, bool in_script) {
    rend->backlog_start = {};
    rend->backlog_start.outer = rend->visbacklogs.len;
    if (in_script)
        scroll_up(rend, 2);
    rend->complete_redraw = true;
    rend->scroll_mode = PROMPT_SCROLL;
    if (!in_script) {
        if (rend->attached_outer != -1)
            prompt->history_counter = prompt->history.len;
        rend->attached_outer = -1;
        rend->selected_outer = rend->attached_outer;
    }
}

static void ensure_prompt_on_screen(Render_State* rend) {
    if (rend->window_rows > 3) {
        Visual_Point backup = rend->backlog_start;
        rend->backlog_start = {};
        rend->backlog_start.outer = rend->visbacklogs.len;
        scroll_up(rend, rend->window_rows - 3);
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
        stop_merging_edits(prompt);
        stop_completing(prompt);
        prompt->text.len = 0;
        prompt->cursor = 0;
        if (prompt->history_counter < history->len) {
            cz::Str hist = history->get(prompt->history_counter);
            insert_before(prompt, prompt->text.len, hist);
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

static void goto_previous_history_match(Prompt_State* prompt, cz::Slice<cz::Str> history) {
    while (1) {
        if (prompt->history_counter == 0) {
            prompt->history_counter = history.len;
            break;
        }
        --prompt->history_counter;
        cz::Str hist = history[prompt->history_counter];
        if (hist.contains_case_insensitive(prompt->text))
            break;
    }
}

static void goto_next_history_match(Prompt_State* prompt, cz::Slice<cz::Str> history) {
    while (1) {
        ++prompt->history_counter;
        if (prompt->history_counter >= history.len) {
            prompt->history_counter = history.len;
            prompt->history_searching = false;
            break;
        }
        cz::Str hist = history[prompt->history_counter];
        if (hist.contains_case_insensitive(prompt->text))
            break;
    }
}

static void finish_prompt_manipulation(Shell_State* shell,
                                       Render_State* rend,
                                       Prompt_State* prompt,
                                       bool doing_merge,
                                       bool doing_completion,
                                       bool doing_history) {
    if (shell) {
        ensure_prompt_on_screen(rend);
        rend->selected_outer = rend->attached_outer;
        rend->scroll_mode = AUTO_SCROLL;
        stop_selecting(rend);
    }
    if (!doing_merge) {
        stop_merging_edits(prompt);
    }
    if (!doing_completion) {
        stop_completing(prompt);
    }
    if (!doing_history) {
        if (prompt->history_searching) {
            prompt->history_counter = prompt->history.len;
            cz::Vector<cz::Str>* history = prompt_history(prompt, rend->attached_outer != -1);
            goto_previous_history_match(prompt, *history);
        }
    }
}

static void run_paste(Prompt_State* prompt) {
    char* clip = SDL_GetClipboardText();
    if (clip) {
        CZ_DEFER(SDL_free(clip));
        size_t len = strlen(clip);
        cz::String str = {clip, len, len};

        cz::strip_carriage_returns(&str);
        while (str.ends_with('\n'))
            str.pop();

        stop_merging_edits(prompt);
        stop_completing(prompt);
        insert_before(prompt, prompt->cursor, str);
    }
}

static bool is_path_sep(char ch) {
#ifdef _WIN32
    return ch == '\\' || ch == '/';
#else
    return ch == '/';
#endif
}

static cz::String deescape(cz::Allocator allocator, cz::Str str) {
    cz::String string = {};
    string.reserve_exact(allocator, str.len);

    for (size_t i = 0; i < str.len; ++i) {
        // @DeescapeOutsideString this entire block is duplicated.
        if (i + 1 < str.len && str[i] == '\\') {
            char c2 = str[i + 1];
            if (c2 == '"' || c2 == '\\' || c2 == '`' || c2 == '$' || c2 == ' ' || c2 == '~' ||
                c2 == '&' || c2 == '*' || c2 == ':') {
                string.push(c2);
                ++i;
            } else if (c2 == '\n') {
                // Skip backslash newline.
                ++i;
            } else {
                string.push('\\');
            }
        } else {
            string.push(str[i]);
        }
    }

    return string;
}

static void start_completing(Prompt_State* prompt, Shell_State* shell) {
    cz::Allocator path_allocator = prompt->completion.results_arena.allocator();

    /////////////////////////////////////////////
    // Get the query
    /////////////////////////////////////////////

    size_t end = prompt->cursor;
    size_t start = end;
    while (start > 0) {
        // TODO handle strings???
        bool escaped = false;
        for (size_t i = start - 1; i-- > 0;) {
            if (prompt->text[i] != '\\')
                break;
            escaped = !escaped;
        }

        char ch = prompt->text[start - 1];
        if (!escaped && (cz::is_space(ch) || ch == ';' || ch == '$'))
            break;
        --start;
    }
    if (start == end)
        return;

    cz::String query = deescape(temp_allocator, prompt->text.slice(start, end));

    prompt->completion.is = true;

    /////////////////////////////////////////////
    // Get all variable names matching the prefix.
    /////////////////////////////////////////////

    if (start > 0 && prompt->text[start - 1] == '$') {
        // Put a dummy element at index 0 so we can tab
        // through back to no completion as a form of undo.
        prompt->completion.results.reserve(cz::heap_allocator(), 1);
        prompt->completion.results.push(query.clone(path_allocator));

        for (size_t i = 0; i < shell->local.variable_names.len; ++i) {
            cz::Str result = shell->local.variable_names[i].str;
            if (cfg.case_sensitive_completion ? result.starts_with(query)
                                              : result.starts_with_case_insensitive(query)) {
                prompt->completion.results.reserve(cz::heap_allocator(), 1);
                prompt->completion.results.push(result.clone_null_terminate(path_allocator));
            }
        }
        prompt->completion.prefix_length = query.len;
        return;
    }

    /////////////////////////////////////////////
    // Split by last slash
    /////////////////////////////////////////////

    const char* slash = query.rfind('/');
#ifdef _WIN32
    {
        const char* backslash = query.rfind('\\');
        if (backslash && (!slash || backslash > slash))
            slash = backslash;
    }
#endif

    cz::Str query_path = (slash ? query.slice_end(slash) : ".");
    cz::Str prefix = (slash ? query.slice_start(slash + 1) : query);
    prompt->completion.prefix_length = prefix.len;

#ifndef _WIN32
    // Deal with absolute paths.
    if (query_path.len == 0)
        query_path = "/";
#endif

    // Put a dummy element at index 0 so we can tab through back to no completion as a form of undo.
    prompt->completion.results.reserve(cz::heap_allocator(), 1);
    prompt->completion.results.push(prefix.clone(path_allocator));

    /////////////////////////////////////////////
    // Get all executables in the path
    /////////////////////////////////////////////

    // TODO: parse the prompt->text and identify if we're at a program name token.
    if (!prompt->text.contains(' ') && !prompt->text.contains('/')) {
#ifdef _WIN32
        cz::Str path_ext;
        (void)get_var(&shell->local, "PATHEXT", &path_ext);
#endif

        cz::String piece = {};
        CZ_DEFER(piece.drop(cz::heap_allocator()));
        cz::Str path;
        if (get_var(&shell->local, "PATH", &path)) {
            while (1) {
#ifdef _WIN32
#define PATH_SEP ';'
#else
#define PATH_SEP ':'
#endif
                cz::Str _piece;
                bool stop = !path.split_excluding(PATH_SEP, &_piece, &path);
                if (stop)
                    _piece = path;

                cz::Directory_Iterator iterator;
                piece.len = 0;
                piece.reserve(cz::heap_allocator(), _piece.len + 2);
                piece.append(_piece);
                if (piece.len > 0 && !is_path_sep(piece.last()))
                    piece.push('/');
                piece.null_terminate();

                int result = iterator.init(piece.buffer);
                if (result <= 0) {
                    if (stop)
                        break;
                    continue;
                }
                CZ_DEFER(iterator.drop());

                // Add this directory to the search list.
                cz::String temp_path = {};
                temp_path.reserve(temp_allocator, path.len + 16);
                temp_path.append(piece);
                size_t temp_path_orig_len = temp_path.len;
                while (1) {
                    cz::Str name = iterator.str_name();
                    if (cfg.case_sensitive_completion ? name.starts_with(prefix)
                                                      : name.starts_with_case_insensitive(prefix)) {
                        temp_path.len = temp_path_orig_len;
                        temp_path.reserve(temp_allocator, name.len + 1);
                        temp_path.append(name);
                        temp_path.null_terminate();

                        bool executable = false;
#ifdef _WIN32
                        executable = has_valid_extension(temp_path, path_ext);
#else
                        executable = is_executable(temp_path.buffer);
#endif
                        if (executable) {
                            cz::String file = {};
                            file.reserve(path_allocator, name.len + 2);
                            escape_arg(name, &file, path_allocator, 1);
                            file.push(' ');
                            file.null_terminate();
                            prompt->completion.results.reserve(cz::heap_allocator(), 1);
                            prompt->completion.results.push(file);
                        }
                    }

                    result = iterator.advance();
                    if (result <= 0)
                        break;
                }

                if (stop)
                    break;
            }
        }

        // Complete builtins.
        if (!slash) {
            for (size_t i = 0; i <= cfg.builtin_level; ++i) {
                cz::Slice<const Builtin> builtins = builtin_levels[i];
                for (size_t j = 0; j < builtins.len; ++j) {
                    const Builtin& builtin = builtins[j];
                    if (cfg.case_sensitive_completion
                            ? builtin.name.starts_with(prefix)
                            : builtin.name.starts_with_case_insensitive(prefix)) {
                        prompt->completion.results.reserve(cz::heap_allocator(), 1);
                        prompt->completion.results.push(builtin.name);
                    }
                }
            }
        }

        // Don't also show file completion because this
        // isn't a valid position to insert a file anyway.
        return;
    }

    /////////////////////////////////////////////
    // Get all files matching the prefix.
    /////////////////////////////////////////////

    // Expand '~/*' to '$HOME/*'.
    cz::String path = {};
    if (query_path == "~" ||
        (query_path.len >= 2 && query_path[0] == '~' && is_path_sep(query_path[1]))) {
        cz::Str home;
        if (get_var(&shell->local, "HOME", &home)) {
            if (home.len > 0 && is_path_sep(home.last()))
                home.len--;
            path.reserve(cz::heap_allocator(), home.len + query_path.len - 1 + 1);
            path.append(home);
            path.append(query_path.slice_start(1));
            path.null_terminate();
            goto skip_absolute;
        }
    }

    cz::path::make_absolute(query_path, get_wd(&shell->local), temp_allocator, &path);
skip_absolute:

    cz::Directory_Iterator iterator;
    int result = iterator.init(path.buffer);
    if (result <= 0)
        return;

    cz::String temp_path = {};
    CZ_DEFER(temp_path.drop(temp_allocator));
    temp_path.reserve(temp_allocator, path.len + 16);
    temp_path.append(path);
    if (!temp_path.ends_with('/'))
        temp_path.push('/');
    size_t temp_path_orig_len = temp_path.len;

    while (1) {
        cz::Str name = iterator.str_name();
        if (cfg.case_sensitive_completion ? name.starts_with(prefix)
                                          : name.starts_with_case_insensitive(prefix)) {
            temp_path.len = temp_path_orig_len;
            temp_path.reserve(temp_allocator, name.len + 2);
            temp_path.append(name);
            temp_path.null_terminate();
            bool is_dir = cz::file::is_directory(temp_path.buffer);

            cz::String file = {};
            file.reserve_exact(path_allocator, name.len + is_dir + 1);
            escape_arg(name, &file, path_allocator, 2);
            if (is_dir)
                file.push('/');
            file.null_terminate();
            prompt->completion.results.reserve(cz::heap_allocator(), 1);
            prompt->completion.results.push(file);
        }

        result = iterator.advance();
        if (result <= 0)
            break;
    }
    iterator.drop();
}

static void stop_merging_edits(Prompt_State* prompt) {
    if (prompt->edit_index > 0) {
        Prompt_Edit* edit = &prompt->edit_history[prompt->edit_index - 1];
        edit->type &= ~PROMPT_EDIT_MERGE;
    }
}

static void stop_completing(Prompt_State* prompt) {
    // Most of the time we aren't completing so just short circuit out.
    if (!prompt->completion.is)
        return;

    prompt->completion.is = false;
    prompt->completion.prefix_length = 0;
    prompt->completion.results_arena.clear();
    prompt->completion.results.len = 0;
    prompt->completion.current = 0;
}

static void delete_forward_1(Prompt_State* prompt) {
    size_t length = 1;
    if (prompt->edit_index > 0) {
        Prompt_Edit* edit = &prompt->edit_history[prompt->edit_index - 1];
        if ((edit->type & PROMPT_EDIT_REMOVE) && (edit->type & PROMPT_EDIT_MERGE) &&
            edit->position == prompt->cursor && edit->value.len + length <= 8 &&
            word_char_category(edit->value.last()) ==
                word_char_category(prompt->text[prompt->cursor])) {
            // The last edit is text input at the same location so merge the edits.
            undo(prompt);
            length += edit->value.len;
            cz::String contents = {(char*)edit->value.buffer, edit->value.len, edit->value.len};
            contents.drop(prompt->edit_arena.allocator());
        }
    }

    remove_after(prompt, prompt->cursor, prompt->cursor + length);
    Prompt_Edit* edit = &prompt->edit_history[prompt->edit_index - 1];
    edit->type |= PROMPT_EDIT_MERGE;
}

static bool handle_prompt_manipulation_commands(Shell_State* shell,
                                                Prompt_State* prompt,
                                                Render_State* rend,
                                                uint16_t mod,
                                                SDL_Keycode key) {
    bool doing_merge = false;
    bool doing_completion = false;
    bool doing_history = false;
    cz::Vector<cz::Str>* history = prompt_history(prompt, rend->attached_outer != -1);

    ///////////////////////////////////////////////////////////////////////
    // Prompt editing commands
    ///////////////////////////////////////////////////////////////////////

    // TODO: call stop_completing???

    if ((mod == KMOD_ALT && key == SDLK_SLASH) ||
        (mod == (KMOD_CTRL | KMOD_SHIFT) && key == SDLK_z)) {
        undo(prompt);
    } else if ((mod == KMOD_CTRL && key == SDLK_SLASH) ||
               (mod == (KMOD_CTRL | KMOD_SHIFT) && key == SDLK_y)) {
        redo(prompt);
    } else if ((mod & ~KMOD_SHIFT) == 0 && key == SDLK_BACKSPACE) {
        if (prompt->cursor > 0) {
            remove_before(prompt, prompt->cursor - 1, prompt->cursor);
        }
    } else if (mod == (KMOD_CTRL | KMOD_ALT) && key == SDLK_BACKSPACE) {
        remove_before(prompt, 0, prompt->cursor);
    } else if ((mod & ~KMOD_SHIFT) == 0 && key == SDLK_DELETE) {
        if (prompt->cursor < prompt->text.len) {
            delete_forward_1(prompt);
            doing_merge = true;
        }
    } else if ((mod == KMOD_ALT && key == SDLK_DELETE) || (mod == KMOD_ALT && key == SDLK_d)) {
        size_t end = prompt->cursor;
        forward_word(prompt->text, &end);
        remove_after(prompt, prompt->cursor, end);
    } else if ((mod == KMOD_CTRL && key == SDLK_BACKSPACE) ||
               (mod == KMOD_ALT && key == SDLK_BACKSPACE)) {
        size_t start = prompt->cursor;
        backward_word(prompt->text, &start);
        remove_before(prompt, start, prompt->cursor);
    } else if (mod == KMOD_CTRL && key == SDLK_k) {
        remove_after(prompt, prompt->cursor, prompt->text.len);
    } else if (mod == (KMOD_CTRL | KMOD_ALT) && key == SDLK_BACKSPACE) {
        remove_before(prompt, 0, prompt->cursor);
    } else if (mod == KMOD_SHIFT && key == SDLK_RETURN) {
        insert_before(prompt, prompt->cursor, "\n");
    } else if (mod == KMOD_ALT && key == SDLK_CARET) {
        const char* ptr = prompt->text.slice_start(prompt->cursor).find('\n');
        if (ptr) {
            start_combo(prompt);
            remove(prompt, ptr - prompt->text.buffer, ptr - prompt->text.buffer + 1);
            insert(prompt, ptr - prompt->text.buffer, " ");
            end_combo(prompt);
        }
    } else if (mod == KMOD_CTRL && key == SDLK_t) {
        if (prompt->cursor < prompt->text.len && prompt->cursor > 0) {
            size_t point = prompt->cursor;
            char ch1 = prompt->text[point - 1];
            char ch2 = prompt->text[point];

            start_combo(prompt);
            remove_after(prompt, point, point + 1);
            remove(prompt, point - 1, point);
            insert(prompt, point - 1, cz::Str{&ch2, 1});
            insert_before(prompt, point, cz::Str{&ch1, 1});
            end_combo(prompt);
        }
    } else if (mod == KMOD_ALT && key == SDLK_t) {
        size_t start1, end1, start2, end2;
        end2 = prompt->cursor;
        forward_word(prompt->text, &end2);
        start2 = end2;
        backward_word(prompt->text, &start2);
        start1 = start2;
        backward_word(prompt->text, &start1);
        end1 = start1;
        forward_word(prompt->text, &end1);

        if (end1 <= start2) {
            cz::Str word1 = prompt->text.slice(start1, end1).clone(temp_allocator);
            cz::Str word2 = prompt->text.slice(start2, end2).clone(temp_allocator);

            start_combo(prompt);
            remove_after(prompt, start2, end2);
            remove(prompt, start1, end1);
            insert(prompt, start1, word2);
            insert_before(prompt, start2 + word2.len - word1.len, word1);
            end_combo(prompt);
        }
    } else if ((mod & ~KMOD_SHIFT) == 0 && key == SDLK_TAB &&
               rend->selected_outer == rend->attached_outer &&
               shell /* no completion for search */ &&
               !prompt->history_searching /* see below SDLK_TAB case */) {
        doing_completion = true;
        stop_merging_edits(prompt);

        if (prompt->completion.is) {
            // Delete previous completion.
            undo(prompt);
        } else {
            start_completing(prompt, shell);
            // Completion won't start if the user isn't at a thing that could conceivably be
            // completed.  Ex. at a blank prompt.  Allow other TAB bindings to take precedence.
            if (!prompt->completion.is)
                return false;

            cz::sort(prompt->completion.results.slice_start(1));
            cz::dedup(&prompt->completion.results);
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

        // Fix capitalization if applicable.
        bool combo = false;
        {
            cz::Str actual = prompt->text.slice(prompt->cursor - prefix, prompt->cursor);
            cz::Str expected = curr.slice_end(prefix);
            if (actual != expected) {
                combo = true;
                start_combo(prompt);

                remove(prompt, prompt->cursor - prefix, prompt->cursor);
                insert(prompt, prompt->cursor - prefix, expected);
            }
        }

        cz::Str ins = curr.slice_start(prefix);
        insert_before(prompt, prompt->cursor, ins);
        if (combo)
            end_combo(prompt);

        // If there are only 0 or 1 results then just stop.
        if (prompt->completion.results.len <= 2)
            stop_completing(prompt);
    } else if ((mod == KMOD_SHIFT && key == SDLK_INSERT) ||
               (mod == (KMOD_CTRL | KMOD_SHIFT) && key == SDLK_v)
#ifdef __APPLE__
               || (mod == KMOD_GUI && key == SDLK_v)
#endif
    ) {
        run_paste(prompt);
    } else if (mod == (KMOD_CTRL | KMOD_SHIFT) && key == SDLK_d) {
        // _d_uplicate the selected line's prompt and paste it at the cursor.
        if (rend->selected_outer != -1) {
            Backlog_State* backlog = rend->visbacklogs[rend->selected_outer];
            // @PromptBacklogEventIndex
            Backlog_Event* start = &backlog->events[2];
            Backlog_Event* end = &backlog->events[3];
            CZ_DEBUG_ASSERT(start->type == BACKLOG_EVENT_START_INPUT);
            CZ_DEBUG_ASSERT(end->type == BACKLOG_EVENT_START_PROCESS);

            // Copy the entire thing to a separate string for simplicity reasons.
            // TODO: copy pieces out of the backlog at a time without temporary allocations.
            cz::String string = {};
            CZ_DEFER(string.drop(cz::heap_allocator()));
            string.reserve_exact(cz::heap_allocator(), end->index - start->index);
            for (uint64_t i = start->index; i < end->index; ++i) {
                string.push(backlog->get(i));
            }

            insert_before(prompt, prompt->cursor, string);
        }
    }

    ///////////////////////////////////////////////////////////////////////
    // History commands
    ///////////////////////////////////////////////////////////////////////

    else if ((mod == 0 && key == SDLK_UP) || (mod == KMOD_CTRL && key == SDLK_p)) {
        if (prompt->history_searching) {
            doing_history = true;
            goto_next_history_match(prompt, *history);
        } else if (prompt->completion.is) {
            doing_completion = true;
            if (prompt->completion.current == 0)
                prompt->completion.current = prompt->completion.results.len;
            prompt->completion.current--;
        } else {
            if (prompt->history_counter > 0) {
                --prompt->history_counter;
                clear_undo_tree(prompt);
                prompt->text.len = 0;
                cz::Str hist = (*history)[prompt->history_counter];
                prompt->text.reserve(cz::heap_allocator(), hist.len);
                prompt->text.append(hist);
                prompt->cursor = prompt->text.len;
            }
        }
    } else if ((mod == 0 && key == SDLK_DOWN) || (mod == KMOD_CTRL && key == SDLK_n)) {
        if (prompt->history_searching) {
            doing_history = true;
            goto_previous_history_match(prompt, *history);
        } else if (prompt->completion.is) {
            doing_completion = true;
            prompt->completion.current++;
            if (prompt->completion.current == prompt->completion.results.len)
                prompt->completion.current = 0;
        } else {
            if (prompt->history_counter < history->len) {
                ++prompt->history_counter;
                clear_undo_tree(prompt);
                prompt->text.len = 0;
                if (prompt->history_counter < history->len) {
                    cz::Str hist = (*history)[prompt->history_counter];
                    prompt->text.reserve(cz::heap_allocator(), hist.len);
                    prompt->text.append(hist);
                }
                prompt->cursor = prompt->text.len;
            }
        }
    } else if (mod == KMOD_CTRL && key == SDLK_r) {
        doing_history = true;
        if (!prompt->history_searching) {
            prompt->history_searching = true;
            prompt->history_counter = history->len;
        }
        goto_previous_history_match(prompt, *history);
    } else if (mod == KMOD_ALT && key == SDLK_r) {
        doing_history = true;
        if (!prompt->history_searching) {
            prompt->history_searching = true;
            prompt->history_counter = history->len;
        }
        goto_next_history_match(prompt, *history);
    } else if ((mod == KMOD_CTRL && key == SDLK_g) || (mod == 0 && key == SDLK_TAB)) {
        resolve_history_searching(prompt, history);
    }

    ///////////////////////////////////////////////////////////////////////
    // Movement commands
    ///////////////////////////////////////////////////////////////////////

    else if ((mod == 0 && key == SDLK_LEFT) || (mod == KMOD_CTRL && key == SDLK_b)) {
        if (prompt->cursor > 0) {
            --prompt->cursor;
        }
    } else if ((mod == 0 && key == SDLK_RIGHT) || (mod == KMOD_CTRL && key == SDLK_f)) {
        if (prompt->cursor < prompt->text.len) {
            ++prompt->cursor;
        }
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
    } else {
        return false;
    }

    finish_prompt_manipulation(shell, rend, prompt, doing_merge, doing_completion, doing_history);
    return true;
}

static void ensure_selected_process_on_screen(Render_State* rend) {
    if (rend->selected_outer <= rend->backlog_start.outer) {
        rend->backlog_start = {};
        rend->backlog_start.outer = rend->selected_outer;
    } else {
        ensure_end_of_selected_process_on_screen(rend, rend->selected_outer, true);
    }
}

static void scroll_to_end_of_selected_process(Render_State* rend, uint64_t selected_outer) {
    rend->backlog_start = {};
    rend->backlog_start.outer = (selected_outer == -1 ? rend->visbacklogs.len : selected_outer + 1);
    int lines = cz::max(rend->window_rows, 6) - 3;
    scroll_up(rend, lines);
}

static void ensure_end_of_selected_process_on_screen(Render_State* rend,
                                                     uint64_t selected_outer,
                                                     bool gotostart) {
    // Go to the earliest point where the entire process / prompt is visible.
    Visual_Point backup = rend->backlog_start;
    scroll_to_end_of_selected_process(rend, selected_outer);

    if ((rend->backlog_start.outer > backup.outer) ||
        (rend->backlog_start.outer == backup.outer && rend->backlog_start.inner > backup.inner)) {
        if (gotostart && rend->backlog_start.outer ==
                             (selected_outer == -1 ? rend->visbacklogs.len : selected_outer)) {
            // The process is really long so go to the start instead.
            rend->backlog_start.inner = 0;
        }
    } else {
        rend->backlog_start = backup;
    }

    // If offscreen then scroll up to fit it.
    if (selected_outer < rend->backlog_start.outer) {
        rend->backlog_start = {};
        rend->backlog_start.outer = selected_outer;
    }
}

static bool is_selected_backlog_on_screen(Render_State* rend, uint64_t selected_outer) {
    if (selected_outer == -1)
        selected_outer = rend->visbacklogs.len;

    if (rend->backlog_start.outer > selected_outer)
        return false;

    Visual_Point backup = rend->backlog_start;
    scroll_down(rend, rend->window_rows - 1);
    Visual_Point new_start = rend->backlog_start;
    rend->backlog_start = backup;

    return (selected_outer <= new_start.outer);
}

static bool handle_scroll_commands(Shell_State* shell,
                                   Prompt_State* prompt,
                                   cz::Slice<Backlog_State*> backlogs,
                                   Render_State* rend,
                                   uint16_t mod,
                                   SDL_Keycode key) {
    Scroll_Mode scroll_mode = MANUAL_SCROLL;
    if ((mod == 0 && key == SDLK_PAGEDOWN) || (mod == KMOD_CTRL && key == SDLK_v)) {
        int lines = cz::max(rend->window_rows, 6) - 3;
        scroll_down(rend, lines);
    } else if ((mod == 0 && key == SDLK_PAGEUP) || (mod == KMOD_ALT && key == SDLK_v)) {
        int lines = cz::max(rend->window_rows, 6) - 3;
        scroll_up(rend, lines);
    } else if ((mod == KMOD_CTRL && key == SDLK_d) &&
               ((rend->attached_outer == -1 && prompt->text.len == 0) ||
                (rend->scroll_mode == MANUAL_SCROLL || rend->scroll_mode == PROMPT_SCROLL))) {
        int lines = rend->window_rows / 2;
        scroll_down(rend, lines);
    } else if (mod == KMOD_CTRL && key == SDLK_u) {
        int lines = rend->window_rows / 2;
        scroll_up(rend, lines);
    } else if (mod == KMOD_ALT && key == SDLK_n) {
        scroll_down(rend, 1);
    } else if (mod == KMOD_ALT && key == SDLK_p) {
        scroll_up(rend, 1);
    } else if ((mod == KMOD_ALT && key == SDLK_LESS) || (mod == KMOD_CTRL && key == SDLK_HOME)) {
        // Goto start of selected process.
        rend->backlog_start = {};
        rend->backlog_start.outer =
            (rend->selected_outer == -1 ? rend->visbacklogs.len : rend->selected_outer);
        scroll_mode = PROMPT_SCROLL;
    } else if ((mod == KMOD_ALT && key == SDLK_GREATER) || (mod == KMOD_CTRL && key == SDLK_END)) {
        // Goto end of selected process.
        scroll_to_end_of_selected_process(rend, rend->selected_outer);
        scroll_mode = AUTO_SCROLL;
    } else if (mod == (KMOD_CTRL | KMOD_ALT) && key == SDLK_b) {
        if (rend->scroll_mode == MANUAL_SCROLL &&
            !is_selected_backlog_on_screen(rend, rend->selected_outer)) {
            // Reset the selection to the first visible window and scroll to its start.
            rend->selected_outer = rend->backlog_start.outer;
            if (rend->selected_outer == rend->visbacklogs.len)
                rend->selected_outer = rend->attached_outer;
            rend->backlog_start.inner = 0;
        } else {
            // Select the process before the currently selected
            // process, or the last process if this is the prompt.
            if (rend->selected_outer == -1 && rend->visbacklogs.len > 0) {
                rend->selected_outer = rend->visbacklogs.len - 1;
            } else if (rend->selected_outer > 0) {
                --rend->selected_outer;
            }
            ensure_selected_process_on_screen(rend);
        }
        scroll_mode = PROMPT_SCROLL;
    } else if (mod == (KMOD_CTRL | KMOD_ALT) && key == SDLK_f) {
        if (rend->scroll_mode == MANUAL_SCROLL &&
            !is_selected_backlog_on_screen(rend, rend->selected_outer)) {
            // Reset the selection to the second visible window and scroll to its start.
            rend->selected_outer = rend->backlog_start.outer + 1;
            if (rend->selected_outer >= rend->visbacklogs.len)
                rend->selected_outer = rend->attached_outer;
            rend->backlog_start.inner = 0;
        } else {
            // Select the next process, or the prompt if this is the last process.
            if (rend->selected_outer != -1 && rend->selected_outer + 1 < rend->visbacklogs.len)
                rend->selected_outer++;
            else
                rend->selected_outer = rend->attached_outer;
            ensure_selected_process_on_screen(rend);
        }
        scroll_mode = PROMPT_SCROLL;
    } else if (mod == (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT) && key == SDLK_b) {
        // Shift backward the selected backlog.
        if (rend->selected_outer != -1 && rend->selected_outer > 0) {
            std::swap(rend->visbacklogs[rend->selected_outer],
                      rend->visbacklogs[rend->selected_outer - 1]);
            if (rend->attached_outer == rend->selected_outer)
                rend->attached_outer = rend->selected_outer - 1;
            else if (rend->attached_outer == rend->selected_outer - 1)
                rend->attached_outer = rend->selected_outer;
            --rend->selected_outer;
        }
        ensure_selected_process_on_screen(rend);
        scroll_mode = PROMPT_SCROLL;
    } else if (mod == (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT) && key == SDLK_f) {
        // Shift forward the selected backlog.
        if (rend->selected_outer != -1 && rend->selected_outer + 1 < rend->visbacklogs.len) {
            std::swap(rend->visbacklogs[rend->selected_outer],
                      rend->visbacklogs[rend->selected_outer + 1]);
            if (rend->attached_outer == rend->selected_outer)
                rend->attached_outer = rend->selected_outer + 1;
            else if (rend->attached_outer == rend->selected_outer + 1)
                rend->attached_outer = rend->selected_outer;
            ++rend->selected_outer;
        }
        ensure_selected_process_on_screen(rend);
        scroll_mode = PROMPT_SCROLL;
    } else if (mod == 0 && key == SDLK_TAB && rend->selected_outer != -1) {
        Backlog_State* backlog = rend->visbacklogs[rend->selected_outer];
        backlog->render_collapsed = !backlog->render_collapsed;
        ensure_selected_process_on_screen(rend);
        if (rend->attached_outer == rend->selected_outer) {
            rend->attached_outer = -1;
            prompt->history_counter = prompt->history.len;
        }
    } else if (mod == KMOD_CTRL && key == SDLK_DELETE && rend->selected_outer != -1) {
        Backlog_State* backlog = rend->visbacklogs[rend->selected_outer];

        if (cfg.control_delete_kill_process) {
            Running_Script* script = lookup_process(shell, backlog->id);
            if (script) {
                kill_process(shell, rend, prompt, backlogs, backlog, script);
            }
            CZ_DEBUG_ASSERT(backlog->refcount == 1);
        }
        backlog_dec_refcount(backlogs, backlog);
        rend->visbacklogs.remove(rend->selected_outer);

        // Detach if attached to killed.
        if (rend->attached_outer == rend->selected_outer) {
            rend->attached_outer = -1;
            prompt->history_counter = prompt->history.len;
        }

        // Fix attached off by one error.
        if (rend->attached_outer != -1 && rend->attached_outer > rend->selected_outer)
            rend->attached_outer--;

        // Fix selected.
        if (rend->selected_outer == rend->visbacklogs.len)
            rend->selected_outer = -1;
    } else {
        return false;
    }

    rend->scroll_mode = scroll_mode;
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

static void finish_hyperlink(Backlog_State* backlog) {
    if (backlog->inside_hyperlink) {
        backlog->inside_hyperlink = false;
        push_backlog_event(backlog, BACKLOG_EVENT_END_HYPERLINK);
    }
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
                                                Prompt_State* prompt) {
    cz::String clip = {};
    CZ_DEFER(clip.drop(temp_allocator));
    for (size_t outer = rend->selection.start.outer; outer <= rend->selection.end.outer; ++outer) {
        if (outer - 1 < rend->visbacklogs.len) {
            Backlog_State* backlog = rend->visbacklogs.get(outer - 1);

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
            cz::Str working_directory = get_wd(&shell->local);
            size_t inner_start = 0;
            size_t inner_end = working_directory.len + prompt->prefix.len + prompt->text.len;
            if (outer == rend->selection.start.outer)
                inner_start = rend->selection.start.inner;
            if (outer == rend->selection.end.outer)
                inner_end = rend->selection.end.inner;

            clip.reserve(temp_allocator, inner_end - inner_start + 2);
            size_t off = 0;
            append_piece(&clip, &off, inner_start, inner_end, working_directory);
            append_piece(&clip, &off, inner_start, inner_end, prompt->prefix);
            append_piece(&clip, &off, inner_start, inner_end, prompt->text);
        }
    }
    clip.null_terminate();
    (void)SDL_SetClipboardText(clip.buffer);
}

static int word_char_category(char ch) {
    switch (ch) {
    case CZ_ALNUM_CASES:
    case '/':
    case '\\':
    case '-':
    case '_':
    case '.':
    case '~':
    case ':':
        return 1;  // Path character.

    case CZ_SPACE_CASES:
        return 2;

    case '\'':
        return 3;
    case '"':
        return 4;

    default:
        return 5;
    }
}

static void expand_selection(Render_State* rend, Shell_State* shell, Prompt_State* prompt) {
    Selection* selection = &rend->selection;
    CZ_DEBUG_ASSERT(selection->start.outer != 0);
    CZ_DEBUG_ASSERT(selection->end.outer != 0);

    if (!selection->expand_word && !selection->expand_line)
        return;

    cz::String prompt_buffer = {};
    CZ_DEFER(prompt_buffer.drop(temp_allocator));

    if (selection->start.outer - 1 == rend->visbacklogs.len ||
        selection->end.outer - 1 == rend->visbacklogs.len) {
        if (rend->attached_outer == -1) {
            cz::append(temp_allocator, &prompt_buffer, get_wd(&shell->local), prompt->prefix);
        } else {
            cz::append(temp_allocator, &prompt_buffer, "> ");
        }
        cz::append(temp_allocator, &prompt_buffer, prompt->text);
    }

    if (selection->expand_word) {
        uint64_t* inner = &selection->start.inner;
        if (selection->start.outer - 1 < rend->visbacklogs.len) {
            Backlog_State* backlog = rend->visbacklogs[selection->start.outer - 1];
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
        if (selection->end.outer - 1 < rend->visbacklogs.len) {
            Backlog_State* backlog = rend->visbacklogs[selection->end.outer - 1];
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
        if (selection->start.outer - 1 < rend->visbacklogs.len) {
            Backlog_State* backlog = rend->visbacklogs[selection->start.outer - 1];
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
        if (selection->end.outer - 1 < rend->visbacklogs.len) {
            Backlog_State* backlog = rend->visbacklogs[selection->end.outer - 1];
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

static void expand_selection_to(Render_State* rend,
                                Shell_State* shell,
                                Prompt_State* prompt,
                                Visual_Tile tile) {
    if (tile.outer == 0) {
        // Select the prompt as well.
        tile.outer = rend->visbacklogs.len + 1;
        tile.inner = get_wd(&shell->local).len + prompt->prefix.len + prompt->text.len;
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

    expand_selection(rend, shell, prompt);

    rend->complete_redraw = true;
}

static bool write_selected_backlog_to_file(Shell_State* shell,
                                           Prompt_State* prompt,
                                           Render_State* rend,
                                           const char* path) {
    cz::Output_File file;
    CZ_DEFER(file.close());
    if (!file.open(path))
        return false;

    if (rend->selected_outer != -1) {
        Backlog_State* backlog = rend->visbacklogs[rend->selected_outer];
        for (size_t i = 0; i + 1 < backlog->buffers.len; ++i) {
            cz::Str buffer = {backlog->buffers[i], BACKLOG_BUFFER_SIZE};
            int64_t result = file.write(buffer);
            if (result != buffer.len)
                return false;
        }
        if (backlog->buffers.len > 0) {
            cz::Str buffer = {backlog->buffers.last(), backlog->length % BACKLOG_BUFFER_SIZE};
            int64_t result = file.write(buffer);
            if (result != buffer.len)
                return false;
            if (backlog->get(backlog->length - 1) != '\n')
                file.write("\n");
        }
    } else {
        int64_t result = file.write(prompt->text);
        if (result != prompt->text.len)
            return false;
        if (prompt->text.len > 0 && prompt->text.last() != '\n')
            file.write("\n");
    }

    return true;
}

static void kill_process(Shell_State* shell,
                         Render_State* rend,
                         Prompt_State* prompt,
                         cz::Slice<Backlog_State*> backlogs,
                         Backlog_State* backlog,
                         Running_Script* script) {
#ifdef TRACY_ENABLE
    // cz::String message =
    //     cz::format(temp_allocator, "End: ", script->root.fg.pipeline.command_line);
    // TracyMessage(message.buffer, message.len);
#endif

    backlog->exit_code = -1;
    backlog->done = true;
    backlog->end = std::chrono::steady_clock::now();
    finish_hyperlink(backlog);
    recycle_process(shell, script);
    backlog_dec_refcount(backlogs, backlog);

    // Detach if the backlog is done.
    if (rend->attached_outer != -1 && rend->visbacklogs[rend->attached_outer]->done) {
        rend->attached_outer = -1;
        prompt->history_counter = prompt->history.len;
    }
}

static bool submit_prompt(Shell_State* shell,
                          Render_State* rend,
                          cz::Vector<Backlog_State*>* backlogs,
                          Prompt_State* prompt,
                          cz::Str command,
                          bool submit,
                          bool allow_attached) {
    Running_Script* script = (allow_attached ? attached_process(shell, rend) : nullptr);
    uint64_t process_id = (script ? script->id : backlogs->len);
    Backlog_State* backlog;
    if (script) {
        backlog = (*backlogs)[process_id];
    } else {
        backlog = push_backlog(backlogs, process_id);
        if (rend) {
            rend->visbacklogs.reserve(cz::heap_allocator(), 1);
            rend->visbacklogs.push(backlog);
            backlog->refcount++;
        }
        push_backlog_event(backlog, BACKLOG_EVENT_START_DIRECTORY);
        append_text(backlog, get_wd(&shell->local));
        push_backlog_event(backlog, BACKLOG_EVENT_START_PROCESS);
        append_text(backlog, prompt->prefix);
    }
    // @PromptBacklogEventIndex
    push_backlog_event(backlog, BACKLOG_EVENT_START_INPUT);
    append_text(backlog, command);
    push_backlog_event(backlog, BACKLOG_EVENT_START_PROCESS);
    append_text(backlog, "\n");

    if (submit) {
        if (script) {
            cz::Str message = cz::format(temp_allocator, command, '\n');
            (void)tty_write(&script->tty, message);
        } else {
            cz::Buffer_Array arena = alloc_arena(shell);
            cz::String command2 = command.clone_null_terminate(arena.allocator());
            if (!run_script(shell, backlog, arena, command2)) {
                backlog_dec_refcount(*backlogs, backlog);
                return false;
            }
        }
    } else {
        if (script) {
            kill_process(shell, rend, prompt, *backlogs, backlog, script);
        } else {
            backlog->done = true;
            backlog->cancelled = true;
            // Don't decrement refcount!
        }
    }
    return true;
}

static void user_submit_prompt(Render_State* rend,
                               Shell_State* shell,
                               cz::Vector<Backlog_State*>* backlogs,
                               Prompt_State* prompt,
                               cz::Str command,
                               bool submit,
                               bool attached) {
    cz::Vector<cz::Str>* history = prompt_history(prompt, attached);

    bool starting_script = (submit && !attached);
    if (starting_script)
        rend->scroll_mode = cfg.on_spawn_scroll_mode;
    else
        rend->scroll_mode = AUTO_SCROLL;

    bool success = submit_prompt(shell, rend, backlogs, prompt, command, submit, attached);
    if (starting_script) {
        // Starting a new script (enter key while detached).
        if (success) {
            if (cfg.on_spawn_attach) {
                rend->attached_outer = rend->visbacklogs.len - 1;
                prompt->history_counter = prompt->stdin_history.len;
            }
        }
        rend->selected_outer = rend->visbacklogs.len - 1;
    }

    // Push the history entry to either the stdin or the shell history list.
    if (command.len > 0) {
        if (history->len == 0 || history->last() != command) {
            history->reserve(cz::heap_allocator(), 1);
            history->push(command.clone(prompt->history_arena.allocator()));
        }
    }

    ensure_prompt_on_screen(rend);
}

static void set_initial_search_position(Search_State* search, Render_State* rend, bool is_forward) {
    if (is_forward) {
        search->outer = rend->backlog_start.outer;
        search->inner = rend->backlog_start.inner;
    } else {
        Visual_Point backup = rend->backlog_start;
        scroll_down1(rend, rend->window_rows);

        search->outer = rend->backlog_start.outer;
        search->inner = rend->backlog_start.inner;

        rend->backlog_start = backup;
    }
}

static int visual_point_compare(const Visual_Point& left, const Visual_Point& right) {
    if (left.outer != right.outer)
        return (int)(left.outer - right.outer);
    return (int)(left.inner - right.inner);
}

static void find_next_search_result(Search_State* search, Render_State* rend, bool is_forward) {
    ZoneScoped;

    Prompt_State* prompt = &search->prompt;
    bool found_result = false;

    search->default_forwards = is_forward;

    /////////////////////////////////////////////
    // Test if current result matches.
    /////////////////////////////////////////////

    if (search->outer < rend->visbacklogs.len) {
        Backlog_State* backlog = rend->visbacklogs[search->outer];
        if (search->inner + prompt->text.len <= backlog->length) {
            uint64_t j = 0;
            for (; j < prompt->text.len; ++j) {
                if (backlog->get(search->inner + j) != prompt->text[j])
                    break;
            }
            if (j == prompt->text.len)
                found_result = true;
        }
    }

    /////////////////////////////////////////////
    // Look for next result.
    /////////////////////////////////////////////

    if (prompt->text.len > 0) {
        // TODO: if we allow reordering backlogs while searching the search->outer/inner
        // will be invalidated, causing this to access memory out of bounds.
        if (is_forward) {
            uint64_t inner = search->inner + 1;
            for (uint64_t o = search->outer; o < rend->visbacklogs.len; ++o, inner = 0) {
                Backlog_State* backlog = rend->visbacklogs[o];
                // TODO: optimize via memchr.
                for (uint64_t i = inner; i + prompt->text.len < backlog->length; ++i) {
                    uint64_t j = 0;
                    for (; j < prompt->text.len; ++j) {
                        if (backlog->get(i + j) != prompt->text[j])
                            break;
                    }
                    if (j != prompt->text.len)
                        continue;

                    // Found a match.
                    search->outer = o;
                    search->inner = i;
                    found_result = true;
                    goto finish_search;
                }
            }
        } else {
            uint64_t o = search->outer;
            uint64_t inner = search->inner;
            if (o == rend->visbacklogs.len && o > 0) {
                o--;
                inner = rend->visbacklogs[o]->length;
            }

            for (; o > 0; o--) {
                Backlog_State* backlog = rend->visbacklogs[o];
                // TODO: optimize via memrchr.
                for (uint64_t i = inner; i-- > 0;) {
                    if (i + prompt->text.len > backlog->length)
                        continue;
                    uint64_t j = 0;
                    for (; j < prompt->text.len; ++j) {
                        if (backlog->get(i + j) != prompt->text[j])
                            break;
                    }
                    if (j != prompt->text.len)
                        continue;

                    // Found a match.
                    search->outer = o;
                    search->inner = i;
                    found_result = true;
                    goto finish_search;
                }

                inner = rend->visbacklogs[o]->length;
            }
        }
    } else {
        set_initial_search_position(search, rend, is_forward);
    }

    /////////////////////////////////////////////
    // Update graphics state
    /////////////////////////////////////////////

finish_search:
    if (found_result && prompt->text.len > 0) {
        Visual_Tile start = {search->outer + 1, search->inner};
        Visual_Tile end = {search->outer + 1, search->inner + prompt->text.len - 1};
        rend->selection.type = SELECT_FINISHED;
        rend->selection.down = start;
        rend->selection.current = end;
        rend->selection.start = start;
        rend->selection.end = end;
        rend->selection.expand_word = 0;
        rend->selection.expand_line = 0;

        rend->scroll_mode = MANUAL_SCROLL;

        // Ensure the search result is on the screen.
        Visual_Point backup = rend->backlog_start;
        rend->backlog_start = {};
        rend->backlog_start.outer = (search->outer != rend->visbacklogs.len ? search->outer : -1);
        rend->backlog_start.inner = search->inner;
        if (is_forward) {
            int lines = cz::max(rend->window_rows, 6) - 3;
            scroll_up(rend, lines);
            if (visual_point_compare(backup, rend->backlog_start) > 0)
                rend->backlog_start = backup;
        } else {
            scroll_up(rend, 3);
            if (visual_point_compare(backup, rend->backlog_start) < 0)
                rend->backlog_start = backup;
        }
    } else {
        rend->selection.type = SELECT_DISABLED;
    }
}

static int process_events(cz::Vector<Backlog_State*>* backlogs,
                          Prompt_State* command_prompt,
                          Search_State* search,
                          Render_State* rend,
                          Shell_State* shell,
                          SDL_Window* window) {
    static uint32_t ignore_key_events_until = 0;

    // If previous KEYDOWN was A-* then track it.
    static SDL_Keycode previous_alt_key = SDLK_UNKNOWN;

    // Tracks if the current key was depressed with an ALT modifier.  This allows for detecting
    // ALT down -> N down -> ALT up -> N up.  SDL will say 'N up' doesn't have an ALT modifier.
    static bool alt_was_down = false;

    int num_events = 0;
    for (SDL_Event event; SDL_PollEvent(&event);) {
        SDL_Keycode is_alt_key = SDLK_UNKNOWN;
        bool alt_is_down = false;
        Prompt_State* prompt = (search->is_searching ? &search->prompt : command_prompt);
        Shell_State* shell2 = (search->is_searching ? nullptr : shell);

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

            is_alt_key = previous_alt_key;
            alt_is_down =
                ((event.key.keysym.mod & KMOD_ALT) && !(event.key.keysym.mod & !KMOD_ALT));

            transform_shift_numbers(&event.key.keysym);

            int mod = (event.key.keysym.mod & ~(KMOD_CAPS | KMOD_NUM));
            // KMOD_ALT = KMOD_LALT | KMOD_RALT so if only one is pressed then mod == KMOD_ALT
            // will be false.  So if one is pressed then pretend all ALT buttons are.
            if (mod & KMOD_ALT)
                mod |= KMOD_ALT;
            if (mod & KMOD_CTRL)
                mod |= KMOD_CTRL;
            if (mod & KMOD_SHIFT)
                mod |= KMOD_SHIFT;

            // Ignore the GUI key.
            mod &= ~KMOD_GUI;

            SDL_Keycode key = event.key.keysym.sym;

            ///////////////////////////////////////////////////////////////////////
            // Search commands
            ///////////////////////////////////////////////////////////////////////

            if ((mod == KMOD_CTRL || mod == KMOD_ALT) && key == SDLK_s) {
                bool is_forward = (mod == KMOD_ALT);

                // Start searching if not.
                if (!search->is_searching) {
                    search->is_searching = true;
                    prompt = &search->prompt;
                    rend->selection.type = SELECT_DISABLED;

                    if (prompt->text.len > 0)
                        remove_before(prompt, 0, prompt->text.len);

                    set_initial_search_position(search, rend, is_forward);
                }

                find_next_search_result(search, rend, is_forward);

                ++num_events;
                continue;
            }

            if (key == SDLK_ESCAPE) {
                if (!search->is_searching && rend->selection.type != SELECT_DISABLED) {
                    stop_selecting(rend);
                } else if (prompt->completion.is || prompt->history_searching) {
                    stop_completing(prompt);
                    prompt->history_searching = false;
                } else if (search->is_searching) {
                    search->is_searching = false;
                    rend->selection.type = SELECT_DISABLED;
                    ++num_events;
                    continue;
                } else if (!cfg.escape_closes) {
                    // Detach and select the prompt.
                    rend->attached_outer = -1;
                    rend->selected_outer = rend->attached_outer;
                    prompt->history_counter = prompt->history.len;
                } else {
                    return -1;
                }
                ++num_events;
                continue;
            }

            if (search->is_searching) {
                size_t old_edit_index = prompt->edit_index;
                if (handle_prompt_manipulation_commands(nullptr, prompt, rend, mod, key)) {
                    if (old_edit_index != prompt->edit_index) {
                        // Restart searching from the end.
                        bool is_forward = search->default_forwards;
                        set_initial_search_position(search, rend, is_forward);
                        find_next_search_result(search, rend, is_forward);
                    }
                    ++num_events;
                    continue;
                }

                if (mod == 0 && (event.key.keysym.sym == SDLK_RETURN ||
                                 event.key.keysym.sym == SDLK_KP_ENTER)) {
                    search->is_searching = false;
                    ++num_events;
                    continue;
                }
                continue;
            }

            if (handle_prompt_manipulation_commands(shell, prompt, rend, mod, key)) {
                ++num_events;
                continue;
            }

            if (handle_scroll_commands(shell, prompt, *backlogs, rend, mod, key)) {
                ++num_events;
                continue;
            }

            if ((mod == KMOD_CTRL && event.key.keysym.sym == SDLK_c) ||
                event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_KP_ENTER) {
                bool submit = (event.key.keysym.sym == SDLK_RETURN) ||
                              (event.key.keysym.sym == SDLK_KP_ENTER);
                bool attached = (rend->attached_outer != -1);

                cz::Vector<cz::Str>* history = prompt_history(prompt, attached);
                resolve_history_searching(prompt, history);

                user_submit_prompt(rend, shell, backlogs, prompt, prompt->text, submit, attached);

                attached = (rend->attached_outer != -1);
                history = prompt_history(prompt, attached);
                prompt->history_counter = history->len;

                stop_merging_edits(prompt);
                stop_completing(prompt);
                prompt->cursor = 0;

                clear_undo_tree(prompt);
                prompt->text.len = 0;
                ++num_events;
                continue;
            }

            if (mod == KMOD_CTRL && key == SDLK_z) {
                rend->scroll_mode = AUTO_SCROLL;
                if (rend->attached_outer == -1) {
                    if (rend->selected_outer != -1 &&
                        !rend->visbacklogs[rend->selected_outer]->done) {
                        // The selected process is still running so attach to it.
                        rend->attached_outer = rend->selected_outer;
                    } else {
                        // Attach to the most recently launched process.
                        for (size_t i = rend->visbacklogs.len; i-- > 0;) {
                            Backlog_State* backlog = rend->visbacklogs[i];
                            if (!backlog->done) {
                                rend->attached_outer = i;
                                break;
                            }
                        }
                    }

                    if (rend->attached_outer != -1) {
                        reorder_attached_to_last(rend);
                        prompt->history_counter = prompt->stdin_history.len;
                    }
                } else {
                    rend->attached_outer = -1;
                    rend->selected_outer = rend->attached_outer;
                    prompt->history_counter = prompt->history.len;
                }
                ++num_events;
                continue;
            }

            if (mod == KMOD_CTRL && key == SDLK_d &&
                (rend->scroll_mode == AUTO_SCROLL || rend->scroll_mode == AUTO_PAGE)) {
                if (prompt->cursor < prompt->text.len) {
                    // Don't call stop_merging_edits!
                    stop_completing(prompt);
                    delete_forward_1(prompt);
                    ++num_events;
                } else if (rend->attached_outer != -1) {
                    Running_Script* script = attached_process(shell, rend);

                    // 4 = EOT / EOF
                    (void)tty_write(&script->tty, "\4");

                    rend->attached_outer = -1;
                    rend->selected_outer = rend->attached_outer;
                    prompt->history_counter = prompt->history.len;
                    ++num_events;
                }
                continue;
            }

            if (mod == KMOD_CTRL && key == SDLK_l) {
                clear_screen(rend, shell, prompt, false);
                ++num_events;
                break;
            }

            // Ctrl + Shift + E - save the selected backlog to a file and open it in $EDITOR.
            if (mod == (KMOD_CTRL | KMOD_SHIFT) && key == SDLK_e) {
                char temp_path[L_tmpnam];
                if (tmpnam(temp_path)) {
                    if (write_selected_backlog_to_file(shell, prompt, rend, temp_path)) {
                        cz::Str command = cz::format(temp_allocator, "__tesh_edit ", temp_path);
                        submit_prompt(shell, nullptr, backlogs, prompt, command, true, false);
                        // TODO reorder attached to last
                    }
                }
                break;
            }

            if (mod == KMOD_ALT && key == SDLK_GREATER) {
                rend->backlog_start = {};
                rend->backlog_start.outer = rend->visbacklogs.len;
                rend->complete_redraw = true;
                ++num_events;

                rend->selected_outer = rend->attached_outer;
                rend->scroll_mode = AUTO_SCROLL;
                int lines = cz::max(rend->window_rows, 3) - 3;
                scroll_up(rend, lines);
                break;
            }

            if ((mod == KMOD_CTRL && key == SDLK_INSERT) ||
                (mod == (KMOD_CTRL | KMOD_SHIFT) && key == SDLK_c)
#ifdef __APPLE__
                || (mod == KMOD_GUI && key == SDLK_c)
#endif
            ) {
                if (rend->selection.type == SELECT_REGION ||
                    rend->selection.type == SELECT_FINISHED)  //
                {
                    // Copy the selected region.
                    rend->selection.type = SELECT_DISABLED;
                    rend->complete_redraw = true;
                    ++num_events;
                    set_clipboard_contents_to_selection(rend, shell, prompt);
                } else if (rend->selected_outer == -1) {
                    // Copy the prompt.
                    cz::String clip = prompt->text.clone_null_terminate(temp_allocator);
                    (void)SDL_SetClipboardText(clip.buffer);
                } else {
                    // Copy the selected backlog.
                    Backlog_State* backlog = rend->visbacklogs[rend->selected_outer];

                    cz::String clip = {};
                    CZ_DEFER(clip.drop(cz::heap_allocator()));
                    clip.reserve_exact(cz::heap_allocator(), backlog->length + 1);

                    // TODO: optimize via appending chunks.
                    for (uint64_t i = 0; i < backlog->length; ++i) {
                        clip.push(backlog->get(i));
                    }
                    clip.null_terminate();

                    (void)SDL_SetClipboardText(clip.buffer);
                }
                break;
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
                rend->grid_is_valid = false;
                ++num_events;
                break;
            }

            if (key == SDLK_LCTRL || key == SDLK_RCTRL) {
                // Redraw because it changes how links are drawn.
                if (rend->grid_is_valid) {
                    int x, y;
                    SDL_GetMouseState(&x, &y);
                    int row = y / rend->font_height;
                    int column = x / rend->font_width;
                    Visual_Tile tile = rend->grid[row * rend->window_cols + column];
                    set_cursor(rend, tile);
                }
                rend->complete_redraw = true;
                ++num_events;
                break;
            }

            // Unbound key.  Try to run a user command.
            const char* key_name = SDL_GetKeyName(key);
            if (key_name && key_name[0]) {
                cz::String name = {};
                cz::append(temp_allocator, &name, "__tesh_");
                if (mod & KMOD_CTRL)
                    cz::append(temp_allocator, &name, "ctrl_");
                if (mod & KMOD_ALT)
                    cz::append(temp_allocator, &name, "alt_");
                if (mod & KMOD_SHIFT)
                    cz::append(temp_allocator, &name, "shift_");
                cz::append(temp_allocator, &name, key_name);

                Shell_Node* value;
                if (get_alias_or_function(&shell->local, name, name, &value)) {
                    cz::String command = {};
                    append_node(temp_allocator, &command, value,
                                /*add_semicolon=*/false);

                    bool attached = (rend->attached_outer != -1);
                    cz::Vector<cz::Str>* history = prompt_history(prompt, attached);
                    bool at_end = (prompt->history_counter == history->len);

                    user_submit_prompt(rend, shell, backlogs, prompt, command, true, false);

                    if (at_end)
                        prompt->history_counter = history->len;

                    ++num_events;
                    break;
                }
            }
        } break;

        case SDL_KEYUP: {
            SDL_Keycode key = event.key.keysym.sym;

            if (key == SDLK_LALT || key == SDLK_RALT) {
                is_alt_key = previous_alt_key;
                alt_is_down = alt_was_down;
            } else {
                if (alt_was_down)
                    is_alt_key = key;
            }

            if (key == SDLK_LCTRL || key == SDLK_RCTRL) {
                // Redraw because it changes how links are drawn.
                if (rend->grid_is_valid) {
                    int x, y;
                    SDL_GetMouseState(&x, &y);
                    int row = y / rend->font_height;
                    int column = x / rend->font_width;
                    Visual_Tile tile = rend->grid[row * rend->window_cols + column];
                    set_cursor(rend, tile);
                }
                rend->complete_redraw = true;
                ++num_events;
                break;
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

#ifdef __APPLE__
            // On mac, A-u will try to push an umlaut on the next character.  We don't
            // care to support this, and prefer to support A-u instead.
            if (previous_alt_key == SDLK_u) {
                uint32_t u32 = cz::utf8::to_utf32((const uint8_t*)event.text.text);

                // Accent by itself means next character is
                // emitted normally so just ignore this one.
                if (u32 == 0x00A8)
                    break;

                switch (u32) {
                    // clang-format off
                case 0x00C4: strcpy(event.text.text, "A"); break;
                case 0x00CB: strcpy(event.text.text, "E"); break;
                case 0x00CF: strcpy(event.text.text, "I"); break;
                case 0x00D6: strcpy(event.text.text, "O"); break;
                case 0x00DC: strcpy(event.text.text, "U"); break;
                case 0x00E4: strcpy(event.text.text, "a"); break;
                case 0x00EB: strcpy(event.text.text, "e"); break;
                case 0x00EF: strcpy(event.text.text, "i"); break;
                case 0x00F6: strcpy(event.text.text, "o"); break;
                case 0x00FC: strcpy(event.text.text, "u"); break;
                case 0x00FF: strcpy(event.text.text, "y"); break;
                case 0x0178: strcpy(event.text.text, "Y"); break;
                    // clang-format on
                }
            }

            // On mac, A-n will try to push an tilde on the next character.  We don't
            // care to support this, and prefer to support A-n instead.
            if (previous_alt_key == SDLK_n) {
                uint32_t u32 = cz::utf8::to_utf32((const uint8_t*)event.text.text);

                // Accent by itself means next character is
                // emitted normally so just ignore this one.
                if (u32 == 0x02DC)
                    break;

                switch (u32) {
                    // clang-format off
                case 0x00C3: strcpy(event.text.text, "A"); break;
                case 0x00D1: strcpy(event.text.text, "N"); break;
                case 0x00D5: strcpy(event.text.text, "O"); break;
                case 0x00E3: strcpy(event.text.text, "a"); break;
                case 0x00F1: strcpy(event.text.text, "n"); break;
                case 0x00F5: strcpy(event.text.text, "a"); break;
                case 0x0128: strcpy(event.text.text, "I"); break;
                case 0x0129: strcpy(event.text.text, "i"); break;
                case 0x0168: strcpy(event.text.text, "U"); break;
                case 0x0169: strcpy(event.text.text, "u"); break;
                    // clang-format on
                }
            }

            // On mac, A-e will try to push an acute accent on the next character.
            // We don't care to support this, and prefer to support A-e instead.
            if (previous_alt_key == SDLK_e) {
                uint32_t u32 = cz::utf8::to_utf32((const uint8_t*)event.text.text);

                // Accent by itself means next character is
                // emitted normally so just ignore this one.
                if (u32 == 0x00B4)
                    break;

                switch (u32) {
                    // clang-format off
                case 0x00C1: strcpy(event.text.text, "A"); break;
                case 0x00C9: strcpy(event.text.text, "E"); break;
                case 0x00CD: strcpy(event.text.text, "I"); break;
                case 0x00D3: strcpy(event.text.text, "O"); break;
                case 0x00DA: strcpy(event.text.text, "U"); break;
                case 0x00DD: strcpy(event.text.text, "Y"); break;
                case 0x00E1: strcpy(event.text.text, "a"); break;
                case 0x00E9: strcpy(event.text.text, "e"); break;
                case 0x00ED: strcpy(event.text.text, "i"); break;
                case 0x00F3: strcpy(event.text.text, "o"); break;
                case 0x00FA: strcpy(event.text.text, "u"); break;
                case 0x00FD: strcpy(event.text.text, "y"); break;
                case 0x0106: strcpy(event.text.text, "C"); break;
                case 0x0107: strcpy(event.text.text, "c"); break;
                case 0x0139: strcpy(event.text.text, "L"); break;
                case 0x013A: strcpy(event.text.text, "l"); break;
                case 0x0143: strcpy(event.text.text, "N"); break;
                case 0x0144: strcpy(event.text.text, "n"); break;
                case 0x0154: strcpy(event.text.text, "R"); break;
                case 0x0155: strcpy(event.text.text, "r"); break;
                case 0x015A: strcpy(event.text.text, "S"); break;
                case 0x015B: strcpy(event.text.text, "s"); break;
                case 0x0179: strcpy(event.text.text, "Z"); break;
                case 0x017A: strcpy(event.text.text, "z"); break;
                case 0x01F4: strcpy(event.text.text, "G"); break;
                case 0x01F5: strcpy(event.text.text, "g"); break;
                case 0x1E30: strcpy(event.text.text, "K"); break;
                case 0x1E31: strcpy(event.text.text, "k"); break;
                case 0x1E3E: strcpy(event.text.text, "M"); break;
                case 0x1E3F: strcpy(event.text.text, "m"); break;
                case 0x1E54: strcpy(event.text.text, "P"); break;
                case 0x1E55: strcpy(event.text.text, "p"); break;
                case 0x1E82: strcpy(event.text.text, "W"); break;
                case 0x1E83: strcpy(event.text.text, "w"); break;
                    // clang-format on
                }
            }

            // On mac, A-i will try to push a circumflex on the next character.
            // We don't care to support this, and prefer to support A-i instead.
            if (previous_alt_key == SDLK_i) {
                uint32_t u32 = cz::utf8::to_utf32((const uint8_t*)event.text.text);

                // Accent by itself means next character is
                // emitted normally so just ignore this one.
                if (u32 == 0x02C6)
                    break;

                switch (u32) {
                    // clang-format off
                case 0x00C2: strcpy(event.text.text, "A"); break;
                case 0x00CA: strcpy(event.text.text, "E"); break;
                case 0x00CE: strcpy(event.text.text, "I"); break;
                case 0x00D4: strcpy(event.text.text, "O"); break;
                case 0x00DB: strcpy(event.text.text, "U"); break;
                case 0x00E2: strcpy(event.text.text, "a"); break;
                case 0x00EA: strcpy(event.text.text, "e"); break;
                case 0x00EE: strcpy(event.text.text, "i"); break;
                case 0x00F4: strcpy(event.text.text, "o"); break;
                case 0x00FB: strcpy(event.text.text, "u"); break;
                case 0x0108: strcpy(event.text.text, "C"); break;
                case 0x0109: strcpy(event.text.text, "c"); break;
                case 0x011C: strcpy(event.text.text, "G"); break;
                case 0x011D: strcpy(event.text.text, "g"); break;
                case 0x0124: strcpy(event.text.text, "H"); break;
                case 0x0125: strcpy(event.text.text, "h"); break;
                case 0x0134: strcpy(event.text.text, "J"); break;
                case 0x0135: strcpy(event.text.text, "j"); break;
                case 0x015C: strcpy(event.text.text, "S"); break;
                case 0x015D: strcpy(event.text.text, "s"); break;
                case 0x0174: strcpy(event.text.text, "W"); break;
                case 0x0175: strcpy(event.text.text, "w"); break;
                case 0x0176: strcpy(event.text.text, "Y"); break;
                case 0x0177: strcpy(event.text.text, "y"); break;
                case 0x1E90: strcpy(event.text.text, "Z"); break;
                case 0x1E91: strcpy(event.text.text, "z"); break;
                    // clang-format on
                }
            }
#endif

            cz::Str text = event.text.text;
            if (prompt->edit_index > 0 && text.len == 1) {
                Prompt_Edit* edit = &prompt->edit_history[prompt->edit_index - 1];
                if (!(edit->type & PROMPT_EDIT_REMOVE) && (edit->type & PROMPT_EDIT_MERGE) &&
                    edit->position + edit->value.len == prompt->cursor &&
                    edit->value.len + text.len <= 8 &&
                    word_char_category(edit->value.last()) == word_char_category(text[0])) {
                    // The last edit is text input at the same location so merge the edits.
                    undo(prompt);
                    cz::String contents = {(char*)edit->value.buffer, edit->value.len,
                                           edit->value.len};
                    contents.reserve_exact(prompt->edit_arena.allocator(), text.len);
                    contents.append(text);
                    text = contents;
                }
            }

            insert_before(prompt, prompt->cursor, text);
            Prompt_Edit* edit = &prompt->edit_history[prompt->edit_index - 1];
            edit->type |= PROMPT_EDIT_MERGE;
            finish_prompt_manipulation(shell2, rend, prompt, true, false, false);
            ++num_events;

            if (search->is_searching) {
                // Restart searching from the end.
                bool is_forward = search->default_forwards;
                set_initial_search_position(search, rend, is_forward);
                find_next_search_result(search, rend, is_forward);
            }
        } break;

        case SDL_TEXTEDITING: {
            if (event.edit.length == 0) {
                is_alt_key = previous_alt_key;
                alt_is_down = alt_was_down;
                break;
            }

            printf("TODO: handle SDL_TEXTEDITING\n");
            printf("Edit: text: %s, start: %" PRIi32 ", length: %" PRIi32 "\n", event.edit.text,
                   event.edit.start, event.edit.length);
        } break;

        case SDL_MOUSEWHEEL: {
            rend->scroll_mode = MANUAL_SCROLL;

            if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                event.wheel.y *= -1;
                event.wheel.x *= -1;
            }

// On linux the horizontal scroll is flipped for some reason.
#ifndef _WIN32
            event.wheel.x *= -1;
#endif
#ifdef __APPLE__
            event.wheel.y *= -1;
#endif

            event.wheel.y *= 4;
            event.wheel.x *= 10;

            SDL_Keymod mods = SDL_GetModState();
            if (mods & KMOD_CTRL) {
                int new_font_size = rend->font_size;
                if (event.wheel.y > 0) {
                    new_font_size += 2;
                } else if (event.wheel.y < 0) {
                    new_font_size = cz::max(new_font_size - 2, 2);
                }
                resize_font(new_font_size, rend);
                rend->grid_is_valid = false;
            } else {
                if (event.wheel.y < 0) {
                    scroll_down(rend, -event.wheel.y);
                } else if (event.wheel.y > 0) {
                    scroll_up(rend, event.wheel.y);
                }
            }

            rend->complete_redraw = true;
            ++num_events;
        } break;

        case SDL_MOUSEBUTTONDOWN: {
            if (event.button.button == SDL_BUTTON_LEFT) {
                SDL_Keymod mods = SDL_GetModState();

                // Control click for open link or attach.
                if (mods & KMOD_CTRL) {
                    if (rend->grid_is_valid) {
                        int x, y;
                        SDL_GetMouseState(&x, &y);
                        int row = y / rend->font_height;
                        int column = x / rend->font_width;
                        Visual_Tile tile = rend->grid[row * rend->window_cols + column];
                        const char* hyperlink = get_hyperlink_at(rend, tile);

                        if (hyperlink) {
                            cz::Str command = cz::format("__tesh_open ", hyperlink);
                            submit_prompt(shell, nullptr, backlogs, prompt, command, true, false);
                            // TODO reorder attached to last
                            break;
                        }

                        // No hyperlink, so instead attach to the clicked backlog.
                        if (rend->attached_outer != -1)
                            prompt->history_counter = prompt->history.len;
                        rend->attached_outer = -1;
                        if (tile.outer != 0 && tile.outer <= rend->visbacklogs.len) {
                            Backlog_State* backlog = rend->visbacklogs[tile.outer - 1];
                            if (!backlog->done) {
                                rend->attached_outer = tile.outer - 1;
                                prompt->history_counter = prompt->stdin_history.len;
                            }
                        }
                        if (rend->attached_outer != -1)
                            reorder_attached_to_last(rend);
                    }
                    break;
                }

                bool holding_shift = (mods & KMOD_SHIFT);
                if (!holding_shift)
                    rend->selected_outer = rend->attached_outer;
                rend->scroll_mode = MANUAL_SCROLL;
                rend->selection.type = SELECT_DISABLED;

                if (!rend->grid_is_valid) {
                    rend->selected_outer = rend->attached_outer;
                    break;
                }

                int row = event.button.y / rend->font_height;
                int column = event.button.x / rend->font_width;
                Visual_Tile tile = rend->grid[row * rend->window_cols + column];

                if (!holding_shift && tile.outer == 0)
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

                if (holding_shift) {
                    expand_selection_to(rend, shell, prompt, tile);
                } else {
                    rend->selected_outer = tile.outer - 1;
                    if (rend->selected_outer == rend->visbacklogs.len)
                        rend->selected_outer = -1;

                    rend->selection.down = tile;
                    rend->selection.current = tile;
                    rend->selection.start = tile;
                    rend->selection.end = tile;

                    expand_selection(rend, shell, prompt);

                    rend->complete_redraw = true;
                    ++num_events;
                }
            } else if (event.button.button == SDL_BUTTON_MIDDLE) {
                run_paste(prompt);
                finish_prompt_manipulation(shell2, rend, prompt, false, false, false);
                ++num_events;
            }
        } break;

        case SDL_MOUSEBUTTONUP: {
            if (event.button.button == SDL_BUTTON_LEFT) {
                if (rend->selection.type == SELECT_REGION) {
                    rend->selection.type = SELECT_FINISHED;
                    if (cfg.on_select_auto_copy) {
                        set_clipboard_contents_to_selection(rend, shell, prompt);
                    }
                } else {
                    rend->selection.type = SELECT_DISABLED;
                }
                rend->complete_redraw = true;
                ++num_events;
            }
        } break;

        case SDL_MOUSEMOTION: {
            // Redraw because links respond to mouse input.
            rend->complete_redraw = true;
            ++num_events;

            if (!rend->grid_is_valid)
                break;

            int row = event.motion.y / rend->font_height;
            int column = event.motion.x / rend->font_width;
            Visual_Tile tile = rend->grid[row * rend->window_cols + column];
            set_cursor(rend, tile);

            if (rend->selection.type == SELECT_DISABLED || rend->selection.type == SELECT_FINISHED)
                break;

            expand_selection_to(rend, shell, prompt, tile);
        } break;
        }

        previous_alt_key = is_alt_key;
        alt_was_down = alt_is_down;
    }
    return num_events;
}

void reorder_attached_to_last(Render_State* rend) {
    Backlog_State* backlog = rend->visbacklogs[rend->attached_outer];
    rend->visbacklogs.remove(rend->attached_outer);
    rend->visbacklogs.push(backlog);
    rend->attached_outer = rend->visbacklogs.len - 1;
    rend->selected_outer = rend->attached_outer;
    rend->backlog_start = {};
    rend->backlog_start.outer = rend->visbacklogs.len;
    int lines = cz::max(rend->window_rows, 3) - 3;
    scroll_up(rend, lines);
}

///////////////////////////////////////////////////////////////////////////////
// History control
///////////////////////////////////////////////////////////////////////////////

void load_history(Prompt_State* prompt, Shell_State* shell) {
    ZoneScoped;

    cz::Input_File file;
    if (!file.open(prompt->history_path.buffer))
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

void save_history(Prompt_State* prompt, Shell_State* shell) {
    ZoneScoped;

    cz::Output_File file;
    if (!file.open(prompt->history_path.buffer))
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
#ifndef NDEBUG
    // Enable in debug builds for quick iteration purposes.
    cfg.escape_closes = true;
#else
    // Disable in release builds to avoid accidental closes.
    cfg.escape_closes = false;
#endif

    cfg.on_spawn_attach = false;
    cfg.on_spawn_scroll_mode = AUTO_PAGE;

    cfg.on_select_auto_copy = true;

    cfg.control_delete_kill_process = true;

    cfg.backlog_info_render_date = false;

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

    cfg.windows_wide_terminal = false;
    cfg.case_sensitive_completion = false;

    static SDL_Color process_colors[] = {
        {0x18, 0, 0, 0xff},    {0, 0x13, 0, 0xff},    {0, 0, 0x20, 0xff},
        {0x11, 0x11, 0, 0xff}, {0, 0x11, 0x11, 0xff}, {0x11, 0, 0x17, 0xff},
    };
    cfg.process_colors = process_colors;

    cfg.theme = solarized_dark;

    cfg.backlog_fg_color = 7;
    cfg.prompt_fg_color = 51;
    cfg.directory_fg_color = 201;
    cfg.info_success_fg_color = 154;
    cfg.info_failure_fg_color = 160;
    cfg.info_running_fg_color = 201;
    cfg.selection_fg_color = 7;
    cfg.selection_bg_color = {0x66, 0x00, 0x66, 0xff};
    cfg.selected_completion_fg_color = 201;
}

static void load_environment_variables(Shell_State* shell) {
    ZoneScoped;

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
                    set_var(&shell->local, key, value);
                    make_env_var(&shell->local, key);
                }
            }
        }
    }

    // Set HOME to the user home directory.
    cz::Str temp;
    if (!get_var(&shell->local, "HOME", &temp)) {
        cz::String home = {};
        if (cz::env::get_home(cz::heap_allocator(), &home)) {
            set_var(&shell->local, "HOME", home);
            make_env_var(&shell->local, "HOME");
        }
    }
#else
    extern char** environ;
    for (char** iter = environ; *iter; ++iter) {
        cz::Str line = *iter;
        cz::Str key, value;
        if (line.split_excluding('=', &key, &value)) {
            if (key.len > 0) {
                set_var(&shell->local, key, value);
                make_env_var(&shell->local, key);
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
    Prompt_State command_prompt = {};
    Search_State search = {};
    Shell_State shell = {};

    load_default_configuration();

    command_prompt.edit_arena.init();
    command_prompt.history_arena.init();
    command_prompt.completion.results_arena.init();
    search.prompt.edit_arena.init();
    search.prompt.history_arena.init();
    search.prompt.completion.results_arena.init();

    cz::Buffer_Array permanent_arena;
    permanent_arena.init();
    permanent_allocator = permanent_arena.allocator();

    cz::Buffer_Array temp_arena;
    temp_arena.init();
    temp_allocator = temp_arena.allocator();

    CZ_DEFER(cleanup_processes(&shell));

    set_program_name(/*fallback=*/argv[0]);
    set_program_directory();

    command_prompt.prefix = " $ ";
    search.prompt.prefix = "SEARCH> ";
    rend.complete_redraw = true;

    rend.font_size = cfg.default_font_size;

    if (argc == 2) {
        cz::set_working_directory(argv[1]);
    }

    {
        cz::String working_directory = {};
        if (!cz::get_working_directory(temp_allocator, &working_directory)) {
            fprintf(stderr, "Failed to get working directory\n");
            return 1;
        }
        set_wd(&shell.local, working_directory);
    }

    load_environment_variables(&shell);
    cz::Str home = {};
    if (get_var(&shell.local, "HOME", &home)) {
        command_prompt.history_path = cz::format(cz::heap_allocator(), home, "/.tesh_history");
    }

    create_null_file();

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

    load_cursors(&rend);

    resize_font(rend.font_size, &rend);

    // Old versions of SDL_ttf don't parse FontLineSkip correctly so we manually set it.
    rend.font_height =
        cz::max(TTF_FontLineSkip(rend.font), (int)(TTF_FontHeight(rend.font) * 1.05f));
    rend.font_width = 10;
    TTF_GlyphMetrics(rend.font, ' ', nullptr, nullptr, nullptr, nullptr, &rend.font_width);

    {
        int w, h;
        SDL_GetWindowSize(window, &w, &h);
        shell.width = w / rend.font_width;
        shell.height = h / rend.font_height;
    }

    rend.attached_outer = -1;
    rend.selected_outer = -1;

    {
        // Start running ~/.teshrc.
        cz::String source_command = cz::format(temp_allocator, "source ~/.teshrc");
        submit_prompt(&shell, &rend, &backlogs, &command_prompt, source_command, true, false);
    }

    load_history(&command_prompt, &shell);
    CZ_DEFER(save_history(&command_prompt, &shell));

    while (1) {
        uint32_t start_frame = SDL_GetTicks();

        temp_arena.clear();

        try {
            int status = process_events(&backlogs, &command_prompt, &search, &rend, &shell, window);
            if (status < 0)
                break;

            bool force_quit = false;
            if (read_process_data(&shell, backlogs, &rend, &command_prompt, &force_quit))
                status = 1;

            if (force_quit)
                break;

            if (rend.complete_redraw || status > 0 || shell.scripts.len > 0 || !rend.grid_is_valid)
                render_frame(window, &rend, &command_prompt, &search, backlogs, &shell);
        } catch (cz::PanicReachedException& ex) {
            fprintf(stderr, "Fatal error: %s\n", ex.what());
            return 1;
        }

        if (shell.scripts.len > 0) {
            // Keep 60fps while any scripts are running.
            const uint32_t frame_length = 1000 / 60;
            uint32_t wanted_end = start_frame + frame_length;
            uint32_t end_frame = SDL_GetTicks();
            if (wanted_end > end_frame) {
                SDL_Delay(wanted_end - end_frame);
            }
        } else {
            // If nothing is running then just wait for the next input event.
            SDL_WaitEvent(NULL);
        }

        FrameMark;
    }

    return 0;
}

void resize_font(int font_size, Render_State* rend) {
    TTF_Font* new_font = NULL;
    SDL_RWops* font_mem;
    int ptsize = (font_size * rend->dpi_scale);
    if (cfg.font_path.len > 0) {
        new_font = TTF_OpenFont(cfg.font_path.buffer, ptsize);
    }
    if (new_font == NULL) {
        // Load the default font instead.
        font_mem = SDL_RWFromConstMem(&UbuntuMonoData[0], sizeof(UbuntuMonoData));
        new_font = TTF_OpenFontRW(font_mem, 0, ptsize);
    }
    if (new_font) {
        close_font(rend);

        rend->font = new_font;
        rend->font_size = font_size;
        // Old versions of SDL_ttf don't parse FontLineSkip correctly so we manually set it.
        rend->font_height =
            cz::max(TTF_FontLineSkip(rend->font), (int)(TTF_FontHeight(rend->font) * 1.05f));
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
    backlog->refcount = 1;
    backlog->arena.init();
    backlog->max_length = cfg.max_length;
    backlog->buffers.reserve(cz::heap_allocator(), 1);
    backlog->buffers.push(buffer);
    backlog->start2 = std::chrono::system_clock::now();
    backlog->start = std::chrono::steady_clock::now();
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

static void load_cursors(Render_State* rend) {
    rend->default_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    // rend->select_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_IBEAM);
    rend->click_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);

    CZ_ASSERT(rend->default_cursor);
    // CZ_ASSERT(rend->select_cursor);
    CZ_ASSERT(rend->click_cursor);
}

static const char* get_hyperlink_at(Render_State* rend, Visual_Tile tile) {
    const char* hyperlink = nullptr;
    if ((SDL_GetModState() & KMOD_CTRL) && tile.outer > 0 &&
        tile.outer - 1 < rend->visbacklogs.len) {
        Backlog_State* backlog = rend->visbacklogs[tile.outer - 1];
        for (size_t event_index = 0; event_index < backlog->events.len; ++event_index) {
            Backlog_Event* event = &backlog->events[event_index];
            if (event->index > tile.inner)
                break;

            if (event->type == BACKLOG_EVENT_START_HYPERLINK) {
                hyperlink = (const char*)event->payload;
            } else if (event->type == BACKLOG_EVENT_END_HYPERLINK) {
                hyperlink = nullptr;
                if (event->index == tile.inner)
                    break;
            }
        }
    }
    return hyperlink;
}

static void set_cursor(Render_State* rend, Visual_Tile tile) {
    const char* hyperlink = get_hyperlink_at(rend, tile);
    SDL_Cursor* cursor = (hyperlink ? rend->click_cursor : rend->default_cursor);
    SDL_SetCursor(cursor);
}

static bool shell_escape_inside(char c) {
    switch (c) {
    case '!':
    case '"':
    case '$':
    case '\\':
    case '`':
        return true;

    default:
        return false;
    }
}

static bool shell_escape_outside(char c) {
    switch (c) {
    case ' ':
    case '!':
    case '"':
    case '#':
    case '$':
    case '&':
    case '\'':
    case '(':
    case ')':
    case '*':
    case ',':
    case ';':
    case '<':
    case '>':
    case '?':
    case '[':
    case '\\':
    case ']':
    case '^':
    case '`':
    case '{':
    case '|':
    case '}':
        return true;

    default:
        return false;
    }
}

void escape_arg(cz::Str arg, cz::String* script, cz::Allocator allocator, size_t reserve_extra) {
    size_t escaped_outside = 0;
    for (size_t i = 0; i < arg.len; ++i) {
        if (shell_escape_outside(arg[i]))
            escaped_outside++;
    }

    script->reserve(allocator, 1 + arg.len + escaped_outside + reserve_extra);

    for (size_t i = 0; i < arg.len; ++i) {
        if (shell_escape_outside(arg[i])) {
            script->push('\\');
        }

        script->push(arg[i]);
    }
}
