#include "shell.hpp"

int64_t Process_Output::write(const void* buffer, size_t len) {
    switch (type) {
    case FILE:
        return v.file.write(buffer, len);
    case BACKLOG:
        append_text(v.backlog, {(const char*)buffer, len});
        return len;
    default:
        CZ_PANIC("unreachable");
    }
}
