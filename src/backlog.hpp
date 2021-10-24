#pragma once

#include <stdint.h>
#include <cz/str.hpp>
#include <cz/vector.hpp>

struct Backlog_Event;

///////////////////////////////////////////////////////////////////////////////

struct Backlog_State {
    cz::Vector<char*> buffers;
    uint64_t length;
    cz::Vector<Backlog_Event> events;
    uint64_t last_process_id;

    char get(size_t index);
};

void append_text(Backlog_State* backlog, uint64_t process_id, cz::Str text);

///////////////////////////////////////////////////////////////////////////////

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

/// Roughly equivalent to appending an event.  Kinda.
void set_backlog_process(Backlog_State* backlog, uint64_t process_id);
