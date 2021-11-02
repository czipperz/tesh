#pragma once

#include <stdint.h>
#include <cz/str.hpp>
#include <cz/vector.hpp>

struct Backlog_Event;

///////////////////////////////////////////////////////////////////////////////

struct Backlog_State {
    uint64_t id;
    cz::Vector<char*> buffers;
    uint64_t length;
    cz::Vector<Backlog_Event> events;

    char get(size_t index);
};

void append_text(Backlog_State* backlog, cz::Str text);

///////////////////////////////////////////////////////////////////////////////

enum Backlog_Event_Type {
    BACKLOG_EVENT_START_INPUT,
    BACKLOG_EVENT_START_PROCESS,
};

struct Backlog_Event {
    uint64_t index;
    uint8_t type;
};
