#include "shell.hpp"

#include <stdlib.h>
#include <Tracy.hpp>
#include <cz/char_type.hpp>
#include <cz/defer.hpp>
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
                            bool make_absolute);

static int run_ls(Process_Output out,
                  cz::String* temp,
                  cz::Str working_directory,
                  cz::Str directory);

void clear_screen(Render_State* rend, Shell_State* shell, cz::Slice<Backlog_State*> backlogs);

///////////////////////////////////////////////////////////////////////////////

bool tick_program(Shell_State* shell,
                  Render_State* rend,
                  cz::Slice<Backlog_State*> backlogs,
                  Backlog_State* backlog,
                  Running_Script* script,
                  Running_Line* line,
                  Running_Program* program,
                  int* exit_code,
                  bool* force_quit) {
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
            if (rounds++ == 1024)
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
            if (rounds++ == 1024)
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
            result = st.file.read_text(st.buffer, 4096, &st.carry);
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
        cz::Str arg;
        if (builtin.args.len >= 2)
            arg = builtin.args[1];
        else if (!get_var(shell, "HOME", &arg)) {
            builtin.exit_code = 1;
            (void)builtin.err.write("cd: No home directory.\n");
            goto finish_builtin;
        }
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
        cz::String temp = {};
        CZ_DEFER(temp.drop(temp_allocator));
        if (builtin.args.len == 1) {
            int result = run_ls(builtin.out, &temp, shell->working_directory, ".");
            if (result < 0) {
                builtin.exit_code = 1;
                (void)builtin.err.write("ls: error\n");
            }
        } else {
            for (size_t i = 1; i < builtin.args.len; ++i) {
                int result = run_ls(builtin.out, &temp, shell->working_directory, builtin.args[i]);
                if (result < 0) {
                    builtin.exit_code = 1;
                    (void)builtin.err.write("ls: error\n");
                    break;
                }
            }
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
                set_alias(shell, key, value);
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
            set_var(shell, st.names[i], st.values[i]);
        }
        goto finish_builtin;
    } break;

    case Running_Program::WHICH: {
        auto& builtin = program->v.builtin;
        auto& st = builtin.st.variables;
        cz::String path = {};
        for (size_t i = 1; i < builtin.args.len; ++i) {
            cz::Str arg = builtin.args[i];
            path.len = 0;
            if (find_in_path(shell, arg, temp_allocator, &path)) {
                (void)builtin.out.write(path);
                (void)builtin.out.write("\n");
            } else {
                builtin.exit_code = 1;
                (void)builtin.err.write("which: Couldn't find ");
                (void)builtin.err.write(arg);
                (void)builtin.err.write("\n");
            }
        }
        goto finish_builtin;
    } break;

    case Running_Program::TRUE_:
        program->v.builtin.exit_code = 0;
        goto finish_builtin;
    case Running_Program::FALSE_:
        program->v.builtin.exit_code = 1;
        goto finish_builtin;

    case Running_Program::EXPORT: {
        auto& builtin = program->v.builtin;
        for (size_t i = 2; i < builtin.args.len; ++i) {
            cz::Str arg = builtin.args[i];
            cz::Str key = arg, value;
            if (arg.split_excluding('=', &key, &value)) {
                if (key.len == 0) {
                    builtin.exit_code = 1;
                    (void)builtin.err.write("export: Empty variable name");
                    (void)builtin.err.write(arg);
                    (void)builtin.err.write("\n");
                    continue;
                }
                set_var(shell, key, value);
            }
            make_env_var(shell, key);
        }
        goto finish_builtin;
    } break;

    case Running_Program::CLEAR: {
        clear_screen(rend, shell, backlogs);
        goto finish_builtin;
    } break;

    case Running_Program::SOURCE: {
        auto& builtin = program->v.builtin;

        if (builtin.args.len <= 1) {
            builtin.exit_code = 1;
            (void)builtin.err.write("source: No file specified\n");
            goto finish_builtin;
        }

        cz::Input_File file;
        if (!file.open(builtin.args[1].buffer)) {
            builtin.exit_code = 1;
            (void)builtin.err.write(
                cz::format(temp_allocator, "source: Couldn't open file ", builtin.args[1], '\n'));
            goto finish_builtin;
        }
        CZ_DEFER(file.close());

        cz::String contents = {};
        read_to_string(file, script->arena.allocator(), &contents);

        Parse_Script subscript = {};
        Error error =
            parse_script(shell, script->arena.allocator(), &subscript, line->on, contents);
        if (error != Error_Success) {
            builtin.exit_code = 1;
            (void)builtin.err.write(
                cz::format(temp_allocator, "source: Error while parsing ", builtin.args[1], '\n'));
            goto finish_builtin;
        }

        Parse_Line* first = script->arena.allocator().clone(subscript.first);
        CZ_ASSERT(first);
        line->on = {};
        line->on.success = first;
        line->on.failure = first;
        goto finish_builtin;
    } break;

    case Running_Program::SLEEP: {
        auto& builtin = program->v.builtin;
        auto& st = program->v.builtin.st.sleep;

        if (builtin.args.len <= 1) {
            builtin.exit_code = 1;
            (void)builtin.err.write("sleep: No time specified\n");
            goto finish_builtin;
        }

        uint64_t max = 0;
        if (!cz::parse(builtin.args[1], &max)) {
            builtin.exit_code = 1;
            (void)builtin.err.write(cz::format(
                temp_allocator, "sleep: Invalid time specified: ", builtin.args[1], "\n"));
            goto finish_builtin;
        }

        auto now = std::chrono::high_resolution_clock::now();
        uint64_t actual = std::chrono::duration_cast<std::chrono::seconds>(now - st.start).count();
        if (actual >= max)
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

///////////////////////////////////////////////////////////////////////////////

static void standardize_arg(const Shell_State* shell,
                            cz::Str arg,
                            cz::Allocator allocator,
                            cz::String* new_wd,
                            bool make_absolute) {
    if (make_absolute) {
        cz::path::make_absolute(arg, shell->working_directory, allocator, new_wd);
    } else {
        new_wd->reserve_exact(allocator, arg.len);
        new_wd->append(arg);
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

///////////////////////////////////////////////////////////////////////////////

static int run_ls(Process_Output out,
                  cz::String* temp,
                  cz::Str working_directory,
                  cz::Str directory) {
    temp->len = 0;
    cz::path::make_absolute(directory, working_directory, temp_allocator, temp);

    cz::Directory_Iterator iterator;
    int result = iterator.init(temp->buffer);
    if (result != 1)
        return result;

    while (1) {
        (void)out.write(iterator.str_name());
        (void)out.write("\n");

        result = iterator.advance();
        if (result <= 0)
            break;
    }

    iterator.drop();
    return result;
}
