#pragma once

#include <stdint.h>
#include <chrono>
#include <cz/buffer_array.hpp>
#include <cz/string.hpp>
#include <cz/vector.hpp>

struct Backlog_Event;

///////////////////////////////////////////////////////////////////////////////

#define BACKLOG_BUFFER_SIZE 4096

struct Backlog_State {
    uint64_t id;

    uint64_t refcount;
    cz::Buffer_Array arena;

    uint64_t max_length;
    cz::Vector<char*> buffers;
    uint64_t length;
    cz::Vector<uint64_t> lines;

    cz::Vector<Backlog_Event> events;
    cz::String escape_backlog;
    uint64_t graphics_rendition;
    bool inside_hyperlink;

    std::chrono::system_clock::time_point start2;
    std::chrono::steady_clock::time_point start, end;
    bool done;
    bool cancelled;  // Subset of done where the backlog wasn't ran.
    int exit_code;

    bool render_collapsed;

    char get(size_t index);
};

void init_backlog(Backlog_State* backlog, uint64_t id, uint64_t max_length);
uint64_t append_text(Backlog_State* backlog, cz::Str text);
void backlog_flush(Backlog_State* backlog);
void cleanup_backlog(cz::Slice<Backlog_State*> backlogs, Backlog_State* backlog);
void backlog_dec_refcount(cz::Slice<Backlog_State*> backlogs, Backlog_State* backlog);

///////////////////////////////////////////////////////////////////////////////

enum Backlog_Event_Type {
    BACKLOG_EVENT_START_INPUT = 0,
    BACKLOG_EVENT_START_PROCESS = 1,
    BACKLOG_EVENT_START_DIRECTORY = 2,
    BACKLOG_EVENT_SET_GRAPHIC_RENDITION = 3,
    BACKLOG_EVENT_START_HYPERLINK = 4,
    BACKLOG_EVENT_END_HYPERLINK = 5,
};

struct Backlog_Event {
    uint64_t index;
    Backlog_Event_Type type;
    uint64_t payload;
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
