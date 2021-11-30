#pragma once

#include <stdint.h>
#include <chrono>
#include <cz/string.hpp>
#include <cz/vector.hpp>

struct Backlog_Event;

///////////////////////////////////////////////////////////////////////////////

struct Backlog_State {
    uint64_t id;

    uint64_t max_length;
    cz::Vector<char*> buffers;
    uint64_t length;
    cz::Vector<uint64_t> lines;

    cz::Vector<Backlog_Event> events;
    cz::String escape_backlog;
    uint64_t graphics_rendition;

    std::chrono::high_resolution_clock::time_point start, end;
    bool done;
    bool cancelled;  // Subset of done where the backlog wasn't ran.
    int exit_code;

    bool render_collapsed;

    char get(size_t index);
};

uint64_t append_text(Backlog_State* backlog, cz::Str text);
void backlog_flush(Backlog_State* backlog);

///////////////////////////////////////////////////////////////////////////////

enum Backlog_Event_Type {
    BACKLOG_EVENT_START_INPUT = 0,
    BACKLOG_EVENT_START_PROCESS = 1,
    BACKLOG_EVENT_START_DIRECTORY = 2,
    BACKLOG_EVENT_SET_GRAPHIC_RENDITION = 3,
    // @BacklogEventTypeBits
};

struct Backlog_Event {
    uint64_t index;
    uint64_t type : 2;  // @BacklogEventTypeBits
    uint64_t payload : 62;
};

///////////////////////////////////////////////////////////////////////////////

// clang-format off
// Note: the payload restricts us to 62 bits.  @BacklogEventTypeBits
#define GR_BOLD             ((uint64_t)0x0000000000000001)
#define GR_UNDERLINE        ((uint64_t)0x0000000000000002)
#define GR_REVERSE          ((uint64_t)0x0000000000000004)
#define GR_BRIGHT           ((uint64_t)0x0000000000000008)
#define GR_FOREGROUND_MASK  ((uint64_t)0x000000000ffffff0)
#define GR_BACKGROUND_MASK  ((uint64_t)0x000ffffff0000000)
#define GR_FOREGROUND_SHIFT 4
#define GR_BACKGROUND_SHIFT 28
// clang-format on
