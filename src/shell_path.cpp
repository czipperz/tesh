#include "shell.hpp"

#include <cz/path.hpp>
#ifndef _WIN32
#include <unistd.h>
#endif

#include <Tracy.hpp>

///////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32
#define PATHSEP '\\'
#else
#define PATHSEP '/'
#endif

#ifdef _WIN32
#define PATH_SPLIT ';'
#else
#define PATH_SPLIT ':'
#endif

///////////////////////////////////////////////////////////////////////////////

static bool is_absolute(cz::Str path);
static bool is_relative(cz::Str path);
#ifdef _WIN32
static bool is_executable_ext(cz::Str path_ext, cz::Allocator allocator, cz::String* path);
#endif

///////////////////////////////////////////////////////////////////////////////

bool find_in_path(Shell_Local* local,
                  cz::Str abbreviation,
                  cz::Allocator allocator,
                  cz::String* full_path) {
    ZoneScoped;

#ifdef _WIN32
    cz::Str path_ext = ".EXE";
    (void)get_var(local, "PATHEXT", &path_ext);
#endif

    // Absolute paths are only looked up verbatim.
    if (is_absolute(abbreviation)) {
        full_path->len = 0;
        full_path->reserve(allocator, abbreviation.len + 1);
        full_path->append(abbreviation);
#ifdef _WIN32
        return is_executable_ext(path_ext, allocator, full_path);
#else
        full_path->null_terminate();
        return is_executable(full_path->buffer);
#endif
    }

    // Relative paths are only looked up relative to the working directory.
    if (is_relative(abbreviation)) {
        // First try just running the exact path.
        cz::Str working_directory = get_wd(local);
        full_path->len = 0;
        full_path->reserve(allocator, working_directory.len + abbreviation.len + 2);
        full_path->append(working_directory);
        full_path->push(PATHSEP);
        full_path->append(abbreviation);
#ifdef _WIN32
        return is_executable_ext(path_ext, allocator, full_path);
#else
        full_path->null_terminate();
        return is_executable(full_path->buffer);
#endif
    }

    cz::Str path;
    if (!get_var(local, "PATH", &path))
        return false;

    while (1) {
        cz::Str piece;
        bool stop = !path.split_excluding(PATH_SPLIT, &piece, &path);
        if (stop)
            piece = path;

        full_path->len = 0;
        full_path->reserve(allocator, piece.len + abbreviation.len + 2);
        full_path->append(piece);
        full_path->push(PATHSEP);
        full_path->append(abbreviation);

#ifdef _WIN32
        if (is_executable_ext(path_ext, allocator, full_path))
            return true;
#else
        full_path->null_terminate();
        if (is_executable(full_path->buffer))
            return true;
#endif

        if (stop)
            break;
    }

    return false;
}

///////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32
static bool is_executable_ext(cz::Str path_ext, cz::Allocator allocator, cz::String* full_path) {
    // If it already has an extension then use it.
    if (has_valid_extension(*full_path, path_ext)) {
        full_path->null_terminate();
        return cz::file::exists(full_path->buffer);
    }

    size_t len = full_path->len;
    cz::Str path_ext_rest = path_ext;
    while (1) {
        cz::Str ext;
        bool stop = !path_ext_rest.split_excluding(PATH_SPLIT, &ext, &path_ext_rest);
        if (stop)
            ext = path_ext_rest;

        full_path->len = len;
        full_path->reserve(allocator, ext.len + 1);
        full_path->append(ext);
        full_path->null_terminate();
        if (cz::file::exists(full_path->buffer))
            return true;

        if (stop)
            break;
    }

    {
        cz::Str ext = ".PS1";
        full_path->len = len;
        full_path->reserve(allocator, ext.len + 1);
        full_path->append(ext);
        full_path->null_terminate();
        if (cz::file::exists(full_path->buffer))
            return true;
    }

    return false;
}

bool has_valid_extension(cz::Str full_path, cz::Str path_ext) {
    const char* dot = full_path.rfind('.');
    if (!dot)
        return false;

    cz::Str ext = full_path.slice_start(dot);
    const char* point = path_ext.find_case_insensitive(ext);
    if (!point)
        return false;

    cz::Str before = path_ext.slice_end(point);
    if (before.len > 0 && !before.ends_with(PATH_SPLIT))
        return false;

    cz::Str after = path_ext.slice_start(point + ext.len);
    if (after.len > 0 && !after.starts_with(PATH_SPLIT))
        return false;

    return true;
}
#endif

///////////////////////////////////////////////////////////////////////////////

static bool is_path_sep(char ch) {
#ifdef _WIN32
    return ch == PATHSEP || ch == '/';
#else
    return ch == PATHSEP;
#endif
}

static bool is_absolute(cz::Str file) {
#ifdef _WIN32
    return file.len >= 3 && cz::is_alpha(file[0]) && file[1] == ':' && is_path_sep(file[2]);
#else
    return file.len >= 1 && file[0] == '/';
#endif
}

static bool is_relative(cz::Str path) {
    // Recognize './' and '../'.
    if (path.len < 2)
        return false;
    if (path[0] != '.')
        return false;
    if (is_path_sep(path[1]))
        return true;
    return path.len >= 3 && path[1] == '.' && is_path_sep(path[2]);
}

#ifndef _WIN32
bool is_executable(const char* path) {
    return access(path, X_OK) == 0;
}
#endif
