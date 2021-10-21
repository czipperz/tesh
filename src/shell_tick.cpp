#include "shell.hpp"

#include <stdlib.h>
#include <Tracy.hpp>
#include <cz/char_type.hpp>
#include <cz/format.hpp>
#include <cz/heap.hpp>
#include <cz/parse.hpp>
#include <cz/path.hpp>

static void standardize_arg(const Shell_State* shell,
                            cz::Str arg,
                            cz::Allocator allocator,
                            cz::String* new_wd,
                            bool make_absolute) {
    // Expand home directory.
    if (arg == "~") {
        cz::Str home = {};
        get_env_var(shell, "HOME", &home);
        new_wd->reserve_exact(allocator, home.len + 1);
        new_wd->append(home);
        new_wd->null_terminate();
    } else if (arg.starts_with("~/")) {
        cz::Str home = {};
        get_env_var(shell, "HOME", &home);
        new_wd->reserve_exact(allocator, home.len + arg.len);
        new_wd->append(home);
        new_wd->push('/');
        new_wd->append(arg.slice_start(2));
        cz::path::flatten(new_wd);
        new_wd->null_terminate();
    } else {
        if (make_absolute) {
            cz::path::make_absolute(arg, shell->working_directory, allocator, new_wd);
        } else {
            new_wd->reserve_exact(allocator, arg.len);
            new_wd->append(arg);
        }
    }

#ifdef _WIN32
    if (make_absolute)
        cz::path::convert_to_forward_slashes(new_wd);
#endif

    if (cz::path::is_absolute(*new_wd)) {
#ifdef _WIN32
        new_wd->get(0) = cz::to_upper(new_wd->get(0));
#endif

#ifdef _WIN32
        bool pop = (new_wd->len > 3 && new_wd->ends_with('/'));
#else
        bool pop = (new_wd->len > 1 && new_wd->ends_with('/'));
#endif
        if (pop) {
            new_wd->pop();
            new_wd->null_terminate();
        }
    }
}

bool tick_program(Shell_State* shell, Running_Program* program, int* exit_code) {
    ZoneScoped;

    switch (program->type) {
    case Running_Program::PROCESS:
        return program->v.process.try_join(exit_code);

    case Running_Program::ECHO: {
        auto& builtin = program->v.builtin;
        auto& st = builtin.st.echo;
        int64_t result = 0;
        int rounds = 0;
        for (; st.outer < builtin.args.len; ++st.outer) {
            // Rate limit to prevent hanging.
            if (rounds++ == 16)
                return false;

            // Write this arg.
            cz::Str arg = builtin.args[st.outer];
            if (st.inner != arg.len) {
                result = builtin.out.write(arg.buffer + st.inner, arg.len - st.inner);
                if (result <= 0)
                    break;

                st.inner += result;
                if (st.inner != arg.len)
                    continue;
            }

            // Write a trailing space.
            if (st.outer + 1 < builtin.args.len) {
                result = builtin.out.write(" ", 1);
                if (result <= 0)
                    break;
                st.inner = 0;
            }
        }

        // Write a final newline.
        if (st.outer == builtin.args.len)
            result = builtin.out.write("\n", 1);

        if (result > 0) {
            CZ_DEBUG_ASSERT(st.outer == builtin.args.len);
            goto finish_builtin;
        }
    } break;

    case Running_Program::CAT: {
        auto& builtin = program->v.builtin;
        auto& st = builtin.st.cat;

        if (builtin.args.len == 1 && builtin.in.is_open()) {
            st.file = builtin.in;
            builtin.in = {};
        }

        int64_t result = 0;
        int rounds = 0;
        while (st.file.is_open() || st.outer < builtin.args.len) {
            // Rate limit to prevent hanging.
            if (rounds++ == 16)
                return false;

            // Write remaining buffer.
            if (st.offset != st.len) {
                result = builtin.out.write(st.buffer + st.offset, st.len - st.offset);
                if (result <= 0)
                    break;

                st.offset += result;
                if (st.offset != st.len)
                    continue;
            }

            // Get a new file if we don't have one.
            if (!st.file.is_open()) {
                cz::Str arg = builtin.args[st.outer];
                if (arg == "-") {
                    st.file = builtin.in;
                    builtin.in = {};
                } else {
                    cz::String path = {};
                    cz::path::make_absolute(arg, shell->working_directory, cz::heap_allocator(),
                                            &path);
                    if (!st.file.open(path.buffer)) {
                        cz::Str message = cz::format("cat: ", arg, ": No such file or directory\n");
                        (void)builtin.err.write(message.buffer, message.len);
                        continue;
                    }
                }
            }

            // Read a new buffer.
            result = st.file.read(st.buffer, 4096);
            if (result <= 0) {
                if (result < 0)
                    break;
                st.file.close();
                st.file = {};
                ++st.outer;
                continue;
            }

            st.offset = 0;
            st.len = result;
        }

        if (result >= 0)
            goto finish_builtin;
    } break;

    case Running_Program::EXIT: {
        auto& builtin = program->v.builtin;
        if (builtin.args.len == 1)
            exit(0);
        int num = 1;
        cz::parse(builtin.args[1], &num);
        exit(num);
    } break;

    case Running_Program::PWD: {
        auto& builtin = program->v.builtin;
        cz::Str wd = shell->working_directory;
        builtin.out.write(wd.buffer, wd.len);
        builtin.out.write("\n", 1);
        goto finish_builtin;
    } break;

    case Running_Program::CD: {
        auto& builtin = program->v.builtin;
        cz::String new_wd = {};
        cz::Str arg = (builtin.args.len >= 2 ? builtin.args[1] : "~");
        standardize_arg(shell, arg, cz::heap_allocator(), &new_wd, /*make_absolute=*/true);
        if (cz::file::is_directory(new_wd.buffer)) {
            shell->working_directory = new_wd;
        } else {
            (void)builtin.err.write("cd: ");
            (void)builtin.err.write(new_wd.buffer, new_wd.len);
            (void)builtin.err.write(": Not a directory\n");
        }
        goto finish_builtin;
    } break;

    default:
        CZ_PANIC("unreachable");
    }
    return false;

finish_builtin:
    auto& builtin = program->v.builtin;
    builtin.in.close();
    builtin.out.close();
    if (builtin.close_err)
        builtin.err.close();
    return true;
}
