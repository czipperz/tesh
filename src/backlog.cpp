#include "backlog.hpp"

#include <cz/heap.hpp>

#define BUFFER_SIZE 4096
#define OUTER_INDEX(index) ((index) >> 12)
#define INNER_INDEX(index) ((index)&0xfff)

char Backlog_State::get(size_t i) {
    return buffers[OUTER_INDEX(i)][INNER_INDEX(i)];
}

void append_text(Backlog_State* backlog, cz::Str text) {
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

void ensure_trailing_newline(Backlog_State* backlog) {
    if (backlog->length == 0)
        return;

    if (backlog->get(backlog->length - 1) != '\n') {
        append_text(backlog, "\n");
    }
}
