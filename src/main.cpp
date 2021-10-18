#include <SDL.h>
#include <SDL_ttf.h>
#include <stdint.h>
#include <stdio.h>
#include <Tracy.hpp>
#include <cz/defer.hpp>
#include <cz/heap.hpp>
#include <cz/string.hpp>
#include <cz/vector.hpp>

struct Render_State {
    TTF_Font* font;
    SDL_Surface* rasterized_characters[256];
    uint64_t render_start = 0;
    SDL_Color background_color;
    SDL_Color foreground_color;
};

struct Buffer_State {
#define BUFFER_SIZE 4096
#define OUTER_INDEX(index) ((index) >> 12)
#define INNER_INDEX(index) ((index)&0xfff)
    cz::Vector<char*> buffers = {};
    uint64_t char_index = 0;
};

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

static SDL_Surface* rasterize_character_cached(Render_State* rend, char ch) {
    uint8_t index = (uint8_t)ch;
    if (rend->rasterized_characters[index])
        return rend->rasterized_characters[index];

    char text[2] = {ch, 0};
    SDL_Surface* surface = rasterize_character(text, rend->font, 0, rend->foreground_color);
    CZ_ASSERT(surface);
    rend->rasterized_characters[index] = surface;
    return surface;
}

static void append_text(Buffer_State* buf, cz::Str text) {
    uint64_t overhang = INNER_INDEX(buf->char_index + text.len);
    uint64_t inner = INNER_INDEX(buf->char_index);
    if (overhang < text.len) {
        uint64_t underhang = text.len - overhang;
        if (underhang > 0) {
            memcpy(buf->buffers.last() + inner, text.buffer + 0, underhang);
        }

        buf->buffers.reserve(cz::heap_allocator(), 1);
        char* buffer = (char*)cz::heap_allocator().alloc({4096, 1});
        CZ_ASSERT(buffer);
        buf->buffers.push(buffer);

        memcpy(buf->buffers.last() + 0, text.buffer + underhang, overhang);
    } else {
        memcpy(buf->buffers.last() + inner, text.buffer, text.len);
    }
    buf->char_index += text.len;
}

static void render_characters(SDL_Surface* window_surface, Render_State* rend, Buffer_State* buf) {
    ZoneScoped;
    int font_height = rasterize_character_cached(rend, 'a')->h;

    SDL_Rect point = {};
    for (uint64_t i = rend->render_start; i < buf->char_index; ++i) {
        char c = buf->buffers[OUTER_INDEX(i)][INNER_INDEX(i)];

        if (c == '\n') {
            point.x = 0;
            point.y += font_height;
            continue;
        }

        SDL_Surface* s = rasterize_character_cached(rend, c);
        if (point.x + s->w > window_surface->w) {
            point.x = 0;
            point.y += font_height;
        }
        // Beyond bottom of screen.
        if (point.y >= window_surface->h)
            break;

        {
            ZoneScopedN("blit_character");
            SDL_BlitSurface(s, NULL, window_surface, &point);
        }

        point.x += s->w;
    }
}

static void render_frame(SDL_Window* window, Render_State* rend, Buffer_State* buf) {
    ZoneScoped;

    SDL_Surface* window_surface = SDL_GetWindowSurface(window);

    uint32_t background = SDL_MapRGB(window_surface->format, rend->background_color.r,
                                     rend->background_color.g, rend->background_color.b);
    {
        ZoneScopedN("draw_background");
        SDL_FillRect(window_surface, NULL, background);
    }

    render_characters(window_surface, rend, buf);

    {
        ZoneScopedN("update_window_surface");
        SDL_UpdateWindowSurface(window);
    }
}

static int process_events(Buffer_State* buf, Render_State* rend) {
    ZoneScoped;

    int num_events = 0;
    for (SDL_Event event; SDL_PollEvent(&event);) {
        switch (event.type) {
        case SDL_QUIT:
            return -1;

        case SDL_WINDOWEVENT:
            // TODO: handle these events.
            ++num_events;
            break;

        case SDL_KEYDOWN:
            if (event.key.keysym.sym == SDLK_ESCAPE)
                return -1;
            if (event.key.keysym.sym == SDLK_RETURN) {
                append_text(buf, "\n");
                ++num_events;
            }
            if (event.key.keysym.sym == SDLK_BACKSPACE) {
                if (buf->char_index > 0) {
                    --buf->char_index;
                    ++num_events;
                }
            }
            break;

        case SDL_TEXTINPUT:
            append_text(buf, event.text.text);
            ++num_events;
            break;
        }
    }
    return num_events;
}

#ifdef _WIN32
const char* font_path = "C:/Windows/Fonts/MesloLGM-Regular.ttf";
#else
const char* font_path = "/usr/share/fonts/TTF/MesloLGMDZ-Regular.ttf";
#endif
const int font_size = 12;

int actual_main(int argc, char** argv) {
    Render_State rend = {};
    Buffer_State buf = {};

    {
        buf.buffers.reserve(cz::heap_allocator(), 1);
        char* buffer = (char*)cz::heap_allocator().alloc({4096, 1});
        CZ_ASSERT(buffer);
        buf.buffers.push(buffer);
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

    rend.background_color = {0x00, 0x00, 0x00, 0xff};
    rend.foreground_color = {0xff, 0xff, 0xff, 0xff};

    while (1) {
        uint32_t start_frame = SDL_GetTicks();

        int status = process_events(&buf, &rend);
        if (status < 0)
            break;

        if (status > 0)
            render_frame(window, &rend, &buf);

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
