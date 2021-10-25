#include "backlog.hpp"

#include <cz/heap.hpp>

#define BUFFER_SIZE 4096
#define OUTER_INDEX(index) ((index) >> 12)
#define INNER_INDEX(index) ((index)&0xfff)

char Backlog_State::get(size_t i) {
    return buffers[OUTER_INDEX(i)][INNER_INDEX(i)];
}

void set_backlog_process(Backlog_State* backlog, uint64_t process_id) {
    Backlog_Event event = {};
    event.index = backlog->length;
    if (process_id == -2) {
        event.type = BACKLOG_EVENT_START_INPUT;
    } else if (process_id == -3) {
        event.type = BACKLOG_EVENT_START_PROMPT;
    } else {
        event.type = BACKLOG_EVENT_START_PROCESS;
        event.v.process_id = process_id;
    }
    backlog->events.reserve(cz::heap_allocator(), 1);
    backlog->events.push(event);
    backlog->last_process_id = process_id;
}

void append_text(Backlog_State* backlog, uint64_t process_id, cz::Str text) {
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

void ensure_trailing_newline(Backlog_State* backlog, uint64_t id) {
    uint64_t end = 0;

    for (size_t e = backlog->events.len; e-- > 0;) {
        Backlog_Event* event = &backlog->events[e];
        if (event->type == BACKLOG_EVENT_START_PROCESS && event->v.process_id == id) {
            if (e + 1 == backlog->events.len)
                end = backlog->length;
            else
                end = event[1].index;
            break;
        }
    }

    if (end == 0)
        return;

    if (backlog->get(end - 1) != '\n') {
        append_text(backlog, id, "\n");
    }
}
