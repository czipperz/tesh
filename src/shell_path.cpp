#include "shell.hpp"

static bool is_executable(const char* path);

bool find_in_path(Shell_State* shell,
                  cz::Str abbreviation,
                  cz::Allocator allocator,
                  cz::String* full_path) {
#ifdef _WIN32
    const char* path_env_var = "Path";
#else
    const char* path_env_var = "PATH";
#endif

    cz::Str path;
    if (!get_env_var(shell, path_env_var, &path))
        return false;

#ifdef _WIN32
    cz::Str path_ext = ".EXE";
    (void)get_env_var(shell, "PATHEXT", &path_ext);
#endif

#ifdef _WIN32
    const char path_split = ';';
#else
    const char path_split = ':';
#endif

    while (1) {
        cz::Str piece;
        bool stop = !path.split_excluding(path_split, &piece, &path);
        if (stop)
            piece = path;

        full_path->len = 0;
        full_path->reserve(allocator, piece.len + abbreviation.len + 2);
        full_path->append(piece);
#ifdef _WIN32
        full_path->push('\\');
#else
        full_path->push('/');
#endif
        full_path->append(abbreviation);
        full_path->null_terminate();

#ifndef _WIN32
        if (is_executable(full_path->buffer))
            return true;
#endif

#ifdef _WIN32
        size_t len = full_path->len;
        cz::Str path_ext_rest = path_ext;
        while (1) {
            cz::Str ext;
            bool stop2 = !path_ext_rest.split_excluding(path_split, &ext, &path_ext_rest);
            if (stop2)
                ext = path_ext_rest;

            full_path->len = len;
            full_path->reserve(allocator, ext.len + 1);
            full_path->append(ext);
            full_path->null_terminate();

            if (is_executable(full_path->buffer))
                return true;

            if (stop2)
                break;
        }
#endif

        if (stop)
            break;
    }

    return false;
}

static bool is_executable(const char* path) {
#ifdef _WIN32
    return cz::file::exists(path);
#else
    return access(path, X_OK) == 0;
#endif
}
