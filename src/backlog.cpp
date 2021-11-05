#include "backlog.hpp"

#include <cz/heap.hpp>

#define BUFFER_SIZE 4096
#define OUTER_INDEX(index) ((index) >> 12)
#define INNER_INDEX(index) ((index)&0xfff)

char Backlog_State::get(size_t i) {
    CZ_DEBUG_ASSERT(i < length);
    return buffers[OUTER_INDEX(i)][INNER_INDEX(i)];
}

static int64_t append_chunk(Backlog_State* backlog, cz::Str text) {
    if (backlog->length == backlog->max_length)
        return 0;

    uint64_t overhang = INNER_INDEX(backlog->length + text.len);
    uint64_t inner = INNER_INDEX(backlog->length);
    if (overhang < text.len) {
        uint64_t underhang = text.len - overhang;
        if (underhang > 0) {
            memcpy(backlog->buffers.last() + inner, text.buffer + 0, underhang);

            if (backlog->length + underhang == backlog->max_length) {
                backlog->length += underhang;
                return underhang;
            }
        }

        backlog->buffers.reserve(cz::heap_allocator(), 1);
        char* buffer = (char*)cz::heap_allocator().alloc({BUFFER_SIZE, 1});
        CZ_ASSERT(buffer);
        backlog->buffers.push(buffer);

        memcpy(backlog->buffers.last() + 0, text.buffer + underhang, overhang);
    } else {
        memcpy(backlog->buffers.last() + inner, text.buffer, text.len);
    }

    backlog->length += text.len;
    return text.len;
}

int64_t append_text(Backlog_State* backlog, cz::Str text) {
    while (1) {
        // Find the first special character.
        size_t chunk_len = text.len;
        chunk_len = text.slice_end(chunk_len).find_index('\r');

        // Append the normal text before it.
        int64_t result = append_chunk(backlog, text.slice_end(chunk_len));
        if (result != chunk_len)
            return result;

        if (chunk_len == text.len)
            break;

        // Handle the special character.
        switch (text[chunk_len]) {
        case '\r':
            // TODO: actually do carriage return?
            // Note: ignore empty lines.
            if (backlog->length > 0 && backlog->get(backlog->length - 1) != '\n') {
                result = append_chunk(backlog, "\n");
                if (result != 1)
                    return result;
            }
            text = text.slice_start(chunk_len + 1);
            break;
        }
    }
}
