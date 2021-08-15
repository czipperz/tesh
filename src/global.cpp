#include "global.hpp"

#include <cz/path.hpp>

cz::Allocator temp_allocator;
cz::Allocator permanent_allocator;

cz::Str program_name;
cz::Str program_directory;

///////////////////////////////////////////////////////////////////////////////

static bool get_program_name(cz::Allocator allocator, cz::String* program_name);

void set_program_name(cz::Str fallback) {
    cz::String name = {};
    if (get_program_name(permanent_allocator, &name)) {
        name.realloc_null_terminate(permanent_allocator);
        program_name = name;
    } else {
        name.drop(permanent_allocator);
        program_name = fallback.clone_null_terminate(permanent_allocator);
    }
}

void set_program_directory() {
    cz::String directory = {};
    directory = program_name.clone(permanent_allocator);
#ifdef _WIN32
    cz::path::convert_to_forward_slashes(&directory);
#endif
    if (cz::path::pop_name(&directory)) {
        directory.realloc_null_terminate(permanent_allocator);
        program_directory = directory;
    } else {
        directory.drop(permanent_allocator);
        program_directory = cz::Str{"."}.clone_null_terminate(permanent_allocator);
    }
}

///////////////////////////////////////////////////////////////////////////////

#include <SDL.h>
#include <cz/format.hpp>

#ifdef _WIN32
#define NOMINMAX
#define WIN32LEANANDMEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

static int64_t get_raw_program_name(char* buffer, size_t cap) {
#ifdef _WIN32
    return GetModuleFileNameA(NULL, buffer, (DWORD)cap);
#else
    return readlink("/proc/self/exe", buffer, cap);
#endif
}

static bool get_program_name(cz::Allocator allocator, cz::String* program_name) {
    program_name->len = 0;
    program_name->reserve_exact(allocator, 256);

    while (1) {
        int64_t count = get_raw_program_name(program_name->buffer, program_name->cap);
        if (count <= 0) {
            // Failure.
            return false;
        } else if ((size_t)count <= program_name->cap) {
            // Success.
            program_name->len = (size_t)count;
            program_name->reserve_exact(allocator, 1);
            program_name->null_terminate();
            return true;
        } else {
            // Try again with more storage.
            program_name->reserve_exact(allocator, program_name->cap * 2);
        }
    }
}
