#include <SDL.h>
#include <SDL_ttf.h>
#include <stdint.h>
#include <stdio.h>
#include <Tracy.hpp>
#include <cz/defer.hpp>
#include <cz/heap.hpp>
#include <cz/process.hpp>
#include <cz/string.hpp>
#include <cz/vector.hpp>

///////////////////////////////////////////////////////////////////////////////
// Type definitions
///////////////////////////////////////////////////////////////////////////////

struct Render_State {
    TTF_Font* font;
    int font_height;
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
};

struct Process_Info {
    uint64_t id;
    cz::Process handle;
    cz::Output_File in;
    cz::Input_File out;
    cz::Carriage_Return_Carry out_carry;
};

struct Process_State {
    cz::Vector<Process_Info> processes;
};

///////////////////////////////////////////////////////////////////////////////
// Data
///////////////////////////////////////////////////////////////////////////////

static SDL_Color process_colors[] = {
    {0x22, 0, 0, 0xff},    {0, 0x22, 0, 0xff},    {0, 0, 0x22, 0xff},
    {0x11, 0x11, 0, 0xff}, {0, 0x11, 0x11, 0xff}, {0x11, 0, 0x11, 0xff},
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

    SDL_Surface* s = rasterize_character_cached(rend, rend->prompt_cache, c, rend->prompt_fg_color);
    if (point->x + s->w > window_surface->w) {
        point->x = 0;
        point->y += rend->font_height;
    }
    // Beyond bottom of screen.
    if (point->y >= window_surface->h)
        return;

    {
        ZoneScopedN("blit_character");
        point->w = s->w;
        point->h = rend->font_height;
        SDL_FillRect(window_surface, point, background);
        SDL_BlitSurface(s, NULL, window_surface, point);
    }

    point->x += s->w;
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
            }
            ++event_index;
        }

        char c = backlog->buffers[OUTER_INDEX(i)][INNER_INDEX(i)];
        render_char(window_surface, rend, point, rend->backlog_cache, background,
                    rend->backlog_fg_color, c);
    }

    rend->backlog_end_index = i;
}

static void render_prompt(SDL_Surface* window_surface, Render_State* rend, Prompt_State* prompt) {
    ZoneScoped;

    SDL_Rect point = rend->backlog_end_point;
    point.w = window_surface->w - point.x;
    point.h = rend->font_height;
    SDL_FillRect(window_surface, &point, SDL_MapRGB(window_surface->format, 0, 0, 0));
    point.x = 0;
    point.y += rend->font_height;

    SDL_Color bg_color = process_colors[prompt->process_id % CZ_DIM(process_colors)];
    uint32_t background = SDL_MapRGB(window_surface->format, bg_color.r, bg_color.g, bg_color.b);

    for (size_t i = 0; i < prompt->prefix.len; ++i) {
        char c = prompt->prefix[i];
        render_char(window_surface, rend, &point, rend->prompt_cache, background,
                    rend->prompt_fg_color, c);
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
    uint32_t foreground = SDL_MapRGB(window_surface->format, 0, 0, 0);
    SDL_FillRect(window_surface, &fill_rect, foreground);

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
                         Prompt_State* prompt) {
    ZoneScoped;

    SDL_Surface* window_surface = SDL_GetWindowSurface(window);

    if (rend->complete_redraw) {
        ZoneScopedN("draw_background");
        SDL_FillRect(window_surface, NULL, SDL_MapRGB(window_surface->format, 0x00, 0x00, 0x00));

        rend->backlog_end_index = rend->backlog_scroll_screen_start;
        rend->backlog_end_point = {};
    }

    render_backlog(window_surface, rend, backlog);
    render_prompt(window_surface, rend, prompt);

    if (rend->complete_redraw) {
        ZoneScopedN("update_window_surface");
        SDL_UpdateWindowSurface(window);
    } else {
        ZoneScopedN("update_window_surface");
        SDL_UpdateWindowSurface(window);
    }

    rend->complete_redraw = false;
}

///////////////////////////////////////////////////////////////////////////////
// Buffer methods
///////////////////////////////////////////////////////////////////////////////

static void set_backlog_process(Backlog_State* backlog, uint64_t process_id) {
    Backlog_Event event = {};
    event.index = backlog->length;
    event.type = BACKLOG_EVENT_START_PROCESS;
    event.v.process_id = process_id;
    backlog->events.reserve(cz::heap_allocator(), 1);
    backlog->events.push(event);
    backlog->last_process_id = process_id;
}

static void append_text(Backlog_State* backlog, cz::Str text, uint64_t process_id) {
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

static bool run_line(Process_State* proc, cz::Str line, uint64_t id) {
    Process_Info process = {};
    process.id = id;

    cz::Process_Options options;
    cz::Process_IO io;
    if (!cz::create_process_pipes(&io, &options))
        return false;
    CZ_DEFER(options.close_all());

    process.in = io.std_in;
    process.out = io.std_out;

    if (!process.in.set_non_blocking())
        return false;
    if (!process.out.set_non_blocking())
        return false;

    if (!process.handle.launch_script(line, options)) {
        process.in.close();
        process.out.close();
        return false;
    }

    proc->processes.reserve(cz::heap_allocator(), 1);
    proc->processes.push(process);
    return true;
}

static bool read_process_data(Process_State* proc, Backlog_State* backlog) {
    static char buffer[4096];
    bool changes = false;
    for (size_t i = 0; i < proc->processes.len; ++i) {
        Process_Info* process = &proc->processes[i];

        int64_t result = 0;
        while (1) {
            result = process->out.read_text(buffer, sizeof(buffer), &process->out_carry);
            if (result <= 0)
                break;
            append_text(backlog, {buffer, (size_t)result}, process->id);
            changes = true;
        }

        if (result == 0) {
            int exit_code = 1;
            if (process->handle.try_join(&exit_code)) {
                process->in.close();
                process->out.close();
                proc->processes.remove(i);
                --i;
            }
        }
    }
    return changes;
}

///////////////////////////////////////////////////////////////////////////////
// User events
///////////////////////////////////////////////////////////////////////////////

static int process_events(Backlog_State* backlog,
                          Prompt_State* prompt,
                          Render_State* rend,
                          Process_State* proc) {
    ZoneScoped;

    int num_events = 0;
    for (SDL_Event event; SDL_PollEvent(&event);) {
        switch (event.type) {
        case SDL_QUIT:
            return -1;

        case SDL_WINDOWEVENT:
            // TODO: handle these events.
            rend->complete_redraw = true;
            ++num_events;
            break;

        case SDL_KEYDOWN: {
            int mod = (event.key.keysym.mod & ~(KMOD_CAPS | KMOD_NUM));
            if (mod & KMOD_ALT)
                mod |= KMOD_ALT;
            if (mod & KMOD_CTRL)
                mod |= KMOD_CTRL;
            if (mod & KMOD_SHIFT)
                mod |= KMOD_SHIFT;
            if (mod & KMOD_GUI)
                mod |= KMOD_GUI;

            if (event.key.keysym.sym == SDLK_ESCAPE)
                return -1;
            if (event.key.keysym.sym == SDLK_RETURN) {
                append_text(backlog, "\n", -1);
                append_text(backlog, prompt->prefix, prompt->process_id);
                append_text(backlog, prompt->text, prompt->process_id);
                append_text(backlog, "\n", prompt->process_id);

                if (!run_line(proc, prompt->text, prompt->process_id)) {
                    append_text(backlog, "Error: failed to execute\n", prompt->process_id);
                }

                prompt->text.len = 0;
                prompt->cursor = 0;
                ++prompt->process_id;
                ++num_events;
            }
            if (event.key.keysym.sym == SDLK_BACKSPACE) {
                if (prompt->cursor > 0) {
                    --prompt->cursor;
                    prompt->text.remove(prompt->cursor);
                    ++num_events;
                }
            }
            if ((mod == 0 && event.key.keysym.sym == SDLK_LEFT) ||
                (mod == KMOD_CTRL && event.key.keysym.sym == SDLK_b)) {
                if (prompt->cursor > 0) {
                    --prompt->cursor;
                    ++num_events;
                }
            }
            if ((mod == 0 && event.key.keysym.sym == SDLK_RIGHT) ||
                (mod == KMOD_CTRL && event.key.keysym.sym == SDLK_f)) {
                if (prompt->cursor < prompt->text.len) {
                    ++prompt->cursor;
                    ++num_events;
                }
            }
            if (mod == KMOD_ALT && event.key.keysym.sym == SDLK_a) {
                prompt->cursor = 0;
                ++num_events;
            }
            if (mod == KMOD_ALT && event.key.keysym.sym == SDLK_e) {
                prompt->cursor = prompt->text.len;
                ++num_events;
            }
        } break;

        case SDL_TEXTINPUT: {
            cz::Str text = event.text.text;
            prompt->text.reserve(cz::heap_allocator(), text.len);
            prompt->text.insert(prompt->cursor, text);
            prompt->cursor += text.len;
            ++num_events;
        } break;
        }
    }
    return num_events;
}

///////////////////////////////////////////////////////////////////////////////
// Configuration
///////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32
const char* font_path = "C:/Windows/Fonts/MesloLGM-Regular.ttf";
#else
const char* font_path = "/usr/share/fonts/TTF/MesloLGMDZ-Regular.ttf";
#endif
int font_size = 12;

///////////////////////////////////////////////////////////////////////////////
// main
///////////////////////////////////////////////////////////////////////////////

int actual_main(int argc, char** argv) {
    Render_State rend = {};
    Backlog_State backlog = {};
    Prompt_State prompt = {};
    Process_State proc = {};

    prompt.prefix = "$ ";
    rend.complete_redraw = true;

    {
        backlog.buffers.reserve(cz::heap_allocator(), 1);
        char* buffer = (char*)cz::heap_allocator().alloc({4096, 1});
        CZ_ASSERT(buffer);
        backlog.buffers.push(buffer);
    }

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

    SDL_Window* window = SDL_CreateWindow("tesh", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                          800, 800, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    CZ_DEFER(SDL_DestroyWindow(window));

    rend.font = open_font(font_path, font_size);
    if (!rend.font) {
        fprintf(stderr, "TTF_OpenFont failed: %s\n", SDL_GetError());
        return 1;
    }
    CZ_DEFER(TTF_CloseFont(rend.font));

    rend.font_height = TTF_FontLineSkip(rend.font);

    rend.backlog_fg_color = {0xdd, 0xdd, 0xdd, 0xff};
    rend.prompt_fg_color = {0xff, 0xff, 0xff, 0xff};

    while (1) {
        uint32_t start_frame = SDL_GetTicks();

        int status = process_events(&backlog, &prompt, &rend, &proc);
        if (status < 0)
            break;

        if (read_process_data(&proc, &backlog))
            status = 1;

        if (status > 0)
            render_frame(window, &rend, &backlog, &prompt);

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
