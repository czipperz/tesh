#include "shell.hpp"

#ifndef _WIN32
#include <sys/select.h>
#endif

int64_t Process_Output::write(const void* buffer, size_t len) {
    switch (type) {
    case FILE:
        if (!v.file.is_open())
            return 0;
        return v.file.write(buffer, len);
    case BACKLOG:
        return append_text(v.backlog, {(const char*)buffer, len});
    default:
        CZ_PANIC("unreachable");
    }
}

static bool should_read(Process_Input* in) {
#ifdef _WIN32
    // TODO: do we need to do this on Windows?
    return true;
#else
    if (in->polling) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(in->file.handle, &rfds);

        // Instant timeout.
        struct timeval timeout = {};

        int result = select(in->file.handle + 1, &rfds, NULL, NULL, &timeout);

        if (result <= 0) {
            CZ_DEBUG_ASSERT(result == 0);
            return false;
        }
    }
    return true;
#endif
}

static int64_t detect_eot(void* buffer, int64_t length, bool* done) {
#ifdef _WIN32
    if (length > 0) {
        // At least right now EOT isn't being auto detected so do it manually.
        cz::Str str = {(char*)buffer, (size_t)length};
        const char* eot = str.find('\4');
        if (eot) {
            *done = true;
            return eot - str.buffer;
        }
    }
#endif
    return length;
}

int64_t Process_Input::read(void* buffer, size_t len) {
    if (done)
        return 0;
    if (!file.is_open())
        return 0;
    if (!should_read(this))
        return -1;

    int64_t length = file.read(buffer, len);
    return detect_eot(buffer, length, &done);
}

int64_t Process_Input::read_text(char* buffer, size_t len, cz::Carriage_Return_Carry* carry) {
    if (done)
        return 0;
    if (!should_read(this))
        return -1;

    int64_t length = file.read_text(buffer, len, carry);
    return detect_eot(buffer, length, &done);
}
