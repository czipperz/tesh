#include "shell.hpp"

#include <stdlib.h>
#include <Tracy.hpp>
#include <cz/char_type.hpp>
#include <cz/directory.hpp>
#include <cz/format.hpp>
#include <cz/heap.hpp>
#include <cz/parse.hpp>
#include <cz/path.hpp>
#include "global.hpp"

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
    } else if (arg.starts_with("~/")) {
        cz::Str home = {};
        get_env_var(shell, "HOME", &home);
        new_wd->reserve_exact(allocator, home.len + arg.len);
        new_wd->append(home);
        new_wd->push('/');
        new_wd->append(arg.slice_start(2));
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

    cz::path::flatten(new_wd);
    new_wd->null_terminate();

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

bool tick_program(Shell_State* shell, Running_Program* program, int* exit_code, bool* force_quit) {
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
            result = builtin.out.write("\n");

        if (result > 0) {
            CZ_DEBUG_ASSERT(st.outer == builtin.args.len);
            goto finish_builtin;
        }
    } break;

    case Running_Program::CAT: {
        auto& builtin = program->v.builtin;
        auto& st = builtin.st.cat;

        if (st.outer == 0) {
            ++st.outer;
            if (builtin.args.len == 1) {
                st.file = builtin.in;
            }
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
                } else {
                    cz::String path = {};
                    cz::path::make_absolute(arg, shell->working_directory, temp_allocator, &path);
                    if (!st.file.open(path.buffer)) {
                        builtin.exit_code = 1;
                        cz::Str message = cz::format("cat: ", arg, ": No such file or directory\n");
                        (void)builtin.err.write(message);
                        ++st.outer;
                        continue;
                    }
                }
            }

            // Read a new buffer.
            result = st.file.read(st.buffer, 4096);
            if (result <= 0) {
                if (result < 0)
                    break;
                if (st.file.handle != builtin.in.handle)
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

    case Running_Program::EXIT:
    case Running_Program::RETURN: {
        auto& builtin = program->v.builtin;
        if (builtin.args.len == 1) {
            builtin.exit_code = 0;
        } else {
            builtin.exit_code = 1;
            if (!cz::parse(builtin.args[1], &builtin.exit_code)) {
                if (program->type == Running_Program::EXIT)
                    (void)builtin.err.write("exit: Invalid code\n");
                else
                    (void)builtin.err.write("return: Invalid code\n");
            }
        }
        if (program->type == Running_Program::EXIT)
            *force_quit = true;
        goto finish_builtin;
    } break;

    case Running_Program::PWD: {
        auto& builtin = program->v.builtin;
        builtin.out.write(shell->working_directory);
        builtin.out.write("\n");
        goto finish_builtin;
    } break;

    case Running_Program::CD: {
        auto& builtin = program->v.builtin;
        cz::String new_wd = {};
        cz::Str arg = (builtin.args.len >= 2 ? builtin.args[1] : "~");
        standardize_arg(shell, arg, temp_allocator, &new_wd, /*make_absolute=*/true);
        if (cz::file::is_directory(new_wd.buffer)) {
            shell->working_directory.len = 0;
            shell->working_directory.reserve(cz::heap_allocator(), new_wd.len + 1);
            shell->working_directory.append(new_wd);
            shell->working_directory.null_terminate();
        } else {
            builtin.exit_code = 1;
            (void)builtin.err.write("cd: ");
            (void)builtin.err.write(new_wd.buffer, new_wd.len);
            (void)builtin.err.write(": Not a directory\n");
        }
        goto finish_builtin;
    } break;

    case Running_Program::LS: {
        auto& builtin = program->v.builtin;
        cz::Directory_Iterator iterator;
        int result = iterator.init(shell->working_directory.buffer);
        if (result == 1) {
            while (1) {
                (void)builtin.out.write(iterator.str_name());
                (void)builtin.out.write("\n");

                result = iterator.advance();
                if (result <= 0)
                    break;
            }
            iterator.drop();
        }

        if (result < 0) {
            builtin.exit_code = 1;
            (void)builtin.err.write("ls: error\n");
        }

        goto finish_builtin;
    } break;

    case Running_Program::ALIAS: {
        auto& builtin = program->v.builtin;
        for (size_t i = 1; i < builtin.args.len; ++i) {
            cz::Str arg = builtin.args[i];
            if (arg.len == 0 || arg[0] == '=') {
                builtin.exit_code = 1;
                (void)builtin.err.write("alias: ");
                (void)builtin.err.write(arg);
                (void)builtin.err.write(": invalid alias name\n");
                continue;
            }

            cz::Str key, value;
            if (arg.split_excluding('=', &key, &value)) {
                shell->alias_names.reserve(cz::heap_allocator(), 1);
                shell->alias_values.reserve(cz::heap_allocator(), 1);
                // TODO: garbage collect / ref count?
                shell->alias_names.push(key.clone(cz::heap_allocator()));
                shell->alias_values.push(value.clone(cz::heap_allocator()));
            } else {
                size_t i = 0;
                for (; i < shell->alias_names.len; ++i) {
                    if (arg == shell->alias_names[i]) {
                        (void)builtin.out.write("alias ");
                        (void)builtin.out.write(shell->alias_names[i]);
                        (void)builtin.out.write("=");
                        (void)builtin.out.write(shell->alias_values[i]);
                        (void)builtin.out.write("\n");
                        break;
                    }
                }
                if (i == shell->alias_names.len) {
                    builtin.exit_code = 1;
                    (void)builtin.err.write("alias: ");
                    (void)builtin.err.write(arg);
                    (void)builtin.err.write(": unbound alias\n");
                }
            }
        }
        goto finish_builtin;
    } break;

    case Running_Program::VARIABLES: {
        auto& st = program->v.builtin.st.variables;
        for (size_t i = 0; i < st.names.len; ++i) {
            set_env_var(shell, st.names[i], st.values[i]);
        }
        goto finish_builtin;
    } break;

    default:
        CZ_PANIC("unreachable");
    }
    return false;

finish_builtin:
    auto& builtin = program->v.builtin;
    *exit_code = builtin.exit_code;
    return true;
}
