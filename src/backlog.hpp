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

    char get(size_t index);
};

void append_text(Backlog_State* backlog, cz::Str text);
void ensure_trailing_newline(Backlog_State* backlog);
