#include <SDL.h>
#include <SDL_ttf.h>
#include <stdint.h>
#include <stdio.h>
#include <Tracy.hpp>
#include <cz/defer.hpp>
#include <cz/heap.hpp>
#include <cz/string.hpp>
#include <cz/vector.hpp>

///////////////////////////////////////////////////////////////////////////////
// Type definitions
///////////////////////////////////////////////////////////////////////////////

struct Render_State {
    TTF_Font* font;
    SDL_Surface* backlog_cache[256];
    SDL_Surface* prompt_cache[256];

    bool complete_redraw;

    uint64_t backlog_scroll_screen_start;
    SDL_Color backlog_bg_color;
    SDL_Color backlog_fg_color;
    uint64_t backlog_end_index;
    SDL_Rect backlog_end_point;

    SDL_Color prompt_bg_color;
    SDL_Color prompt_fg_color;
};

struct Backlog_State {
#define BUFFER_SIZE 4096
#define OUTER_INDEX(index) ((index) >> 12)
#define INNER_INDEX(index) ((index)&0xfff)
    cz::Vector<char*> buffers;
    uint64_t length;
};

struct Prompt_State {
    cz::Str prefix;
    cz::String text;
    size_t cursor;
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

static void render_backlog(SDL_Surface* window_surface,
                           Render_State* rend,
                           Backlog_State* backlog) {
    ZoneScoped;
    int font_height = TTF_FontLineSkip(rend->font);
    SDL_Rect* point = &rend->backlog_end_point;
    uint64_t i = rend->backlog_end_index;
    for (; i < backlog->length; ++i) {
        char c = backlog->buffers[OUTER_INDEX(i)][INNER_INDEX(i)];

        if (c == '\n') {
            point->x = 0;
            point->y += font_height;
            continue;
        }

        SDL_Surface* s =
            rasterize_character_cached(rend, rend->backlog_cache, c, rend->backlog_fg_color);
        if (point->x + s->w > window_surface->w) {
            point->x = 0;
            point->y += font_height;
        }
        // Beyond bottom of screen.
        if (point->y >= window_surface->h)
            break;

        if (point->x == 0) {
            uint32_t background = SDL_MapRGB(window_surface->format, rend->backlog_bg_color.r,
                                             rend->backlog_bg_color.g, rend->backlog_bg_color.b);
            SDL_Rect fill_rect = {0, point->y, window_surface->w, font_height};
            SDL_FillRect(window_surface, &fill_rect, background);
        }

        {
            ZoneScopedN("blit_character");
            SDL_BlitSurface(s, NULL, window_surface, point);
        }

        point->x += s->w;
    }
    rend->backlog_end_index = i;
}

static void render_prompt_char(SDL_Surface* window_surface,
                               Render_State* rend,
                               int font_height,
                               SDL_Rect* point,
                               char c) {
    if (c == '\n') {
        point->x = 0;
        point->y += font_height;
        return;
    }

    SDL_Surface* s = rasterize_character_cached(rend, rend->prompt_cache, c, rend->prompt_fg_color);
    if (point->x + s->w > window_surface->w) {
        point->x = 0;
        point->y += font_height;
    }
    // Beyond bottom of screen.
    if (point->y >= window_surface->h)
        CZ_PANIC("unimplemented");

    {
        ZoneScopedN("blit_character");
        SDL_BlitSurface(s, NULL, window_surface, point);
    }

    point->x += s->w;
}

static void render_prompt(SDL_Surface* window_surface, Render_State* rend, Prompt_State* prompt) {
    ZoneScoped;
    int font_height = TTF_FontLineSkip(rend->font);

    SDL_Rect point = rend->backlog_end_point;
    point.x = 0;
    point.y += font_height;

    SDL_Rect prompt_rect = {0, point.y, window_surface->w, window_surface->h - point.y};
    uint32_t background = SDL_MapRGB(window_surface->format, rend->prompt_bg_color.r,
                                     rend->prompt_bg_color.g, rend->prompt_bg_color.b);
    SDL_FillRect(window_surface, &prompt_rect, background);

    for (size_t i = 0; i < prompt->prefix.len; ++i) {
        char c = prompt->prefix[i];
        render_prompt_char(window_surface, rend, font_height, &point, c);
    }
    for (size_t i = 0; i < prompt->text.len; ++i) {
        if (prompt->cursor == i) {
            SDL_Rect fill_rect = {point.x, point.y, 2, font_height};
            uint32_t foreground = SDL_MapRGB(window_surface->format, rend->prompt_fg_color.r,
                                             rend->prompt_fg_color.g, rend->prompt_fg_color.b);
            SDL_FillRect(window_surface, &fill_rect, foreground);
        }
        char c = prompt->text[i];
        render_prompt_char(window_surface, rend, font_height, &point, c);
    }
    if (prompt->cursor == prompt->text.len) {
        SDL_Rect fill_rect = {point.x, point.y, 2, font_height};
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

    uint32_t background = SDL_MapRGB(window_surface->format, rend->backlog_bg_color.r,
                                     rend->backlog_bg_color.g, rend->backlog_bg_color.b);
    if (rend->complete_redraw) {
        ZoneScopedN("draw_background");
        SDL_FillRect(window_surface, NULL, background);

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

static void append_text(Backlog_State* backlog, cz::Str text) {
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
// Process user events
///////////////////////////////////////////////////////////////////////////////

static int process_events(Backlog_State* backlog, Prompt_State* prompt, Render_State* rend) {
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

        case SDL_KEYDOWN:
            if (event.key.keysym.sym == SDLK_ESCAPE)
                return -1;
            if (event.key.keysym.sym == SDLK_RETURN) {
                prompt->text.len = 0;
                prompt->cursor = 0;
                ++num_events;
            }
            if (event.key.keysym.sym == SDLK_BACKSPACE) {
                if (prompt->cursor > 0) {
                    --prompt->cursor;
                    prompt->text.remove(prompt->cursor);
                    ++num_events;
                }
            }
            if (event.key.keysym.sym == SDLK_LEFT) {
                if (prompt->cursor > 0) {
                    --prompt->cursor;
                    ++num_events;
                }
            }
            if (event.key.keysym.sym == SDLK_RIGHT) {
                if (prompt->cursor < prompt->text.len) {
                    ++prompt->cursor;
                    ++num_events;
                }
            }
            break;

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

    rend.backlog_bg_color = {0x22, 0x22, 0x22, 0xff};
    rend.backlog_fg_color = {0xdd, 0xdd, 0xdd, 0xff};
    rend.prompt_bg_color = {0x00, 0x00, 0x00, 0xff};
    rend.prompt_fg_color = {0xff, 0xff, 0xff, 0xff};

    uint32_t spaghetti_time = SDL_GetTicks();

    while (1) {
        uint32_t start_frame = SDL_GetTicks();

        int status = process_events(&backlog, &prompt, &rend);
        if (status < 0)
            break;

        if (spaghetti_time - start_frame >= 250) {
            spaghetti_time += 250;
            // TODO: remove this test.
            append_text(&backlog, "sandwichsandwichsandwichsandwich");
            status = 1;
        }

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
