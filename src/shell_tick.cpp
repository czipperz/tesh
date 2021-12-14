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
#include "prompt.hpp"

///////////////////////////////////////////////////////////////////////////////
// Forward declarations
///////////////////////////////////////////////////////////////////////////////

static void read_tty_output(Backlog_State* backlog, Pseudo_Terminal* tty, bool cap_read_calls);

static void tick_pipeline(Shell_State* shell,
                          Shell_Local* local,
                          Render_State* rend,
                          Prompt_State* prompt,
                          cz::Slice<Backlog_State*> backlogs,
                          Backlog_State* backlog,
                          Running_Pipeline* pipeline,
                          Pseudo_Terminal* tty,
                          bool* force_quit);

static bool tick_program(Shell_State* shell,
                         Shell_Local* local,
                         Render_State* rend,
                         Prompt_State* prompt,
                         cz::Slice<Backlog_State*> backlogs,
                         Backlog_State* backlog,
                         cz::Allocator allocator,
                         Running_Program* program,
                         Pseudo_Terminal* tty,
                         int* exit_code,
                         bool* force_quit);

static void standardize_arg(const Shell_Local* local,
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
// Tick running node
///////////////////////////////////////////////////////////////////////////////

bool tick_running_node(Shell_State* shell,
                       cz::Slice<Backlog_State*> backlogs,
                       Render_State* rend,
                       Prompt_State* prompt,
                       Running_Node* node,
                       Pseudo_Terminal* tty,
                       Backlog_State* backlog,
                       bool* force_quit) {
    size_t starting_length = backlog->length;
    for (size_t b = 0; b < node->bg.len; ++b) {
        Running_Pipeline* line = &node->bg[b];
        tick_pipeline(shell, node->local, rend, prompt, backlogs, backlog, line, tty, force_quit);
        if (line->programs.len == 0) {
            finish_line(shell, *tty, node, backlog, line, /*background=*/true);
            --b;
        }
    }

    tick_pipeline(shell, node->local, rend, prompt, backlogs, backlog, &node->fg, tty, force_quit);

    if (*force_quit)
        return true;

    read_tty_output(backlog, tty, /*cap_read_calls=*/true);

    if (node->fg.programs.len == 0 && !node->fg_finished) {
        bool started = finish_line(shell, *tty, node, backlog, &node->fg,
                                   /*background=*/false);
        if (!started)
            node->fg_finished = true;

        // Rerun to prevent long scripts from only doing one command per frame.
        // TODO: rate limit to prevent big scripts (with all builtins) from hanging.
        return true;
    }

    return false;
}

///////////////////////////////////////////////////////////////////////////////

static void tick_pipeline(Shell_State* shell,
                          Shell_Local* local,
                          Render_State* rend,
                          Prompt_State* prompt,
                          cz::Slice<Backlog_State*> backlogs,
                          Backlog_State* backlog,
                          Running_Pipeline* pipeline,
                          Pseudo_Terminal* tty,
                          bool* force_quit) {
    cz::Allocator allocator = pipeline->arena.allocator();
    for (size_t p = 0; p < pipeline->programs.len; ++p) {
        Running_Program* program = &pipeline->programs[p];
        int exit_code = 1;
        if (tick_program(shell, local, rend, prompt, backlogs, backlog, allocator, program, tty,
                         &exit_code, force_quit)) {
            if (!pipeline->has_exit_code && p + 1 == pipeline->programs.len) {
                pipeline->has_exit_code = true;
                pipeline->last_exit_code = exit_code;
            }
            backlog->end = std::chrono::high_resolution_clock::now();
            pipeline->programs.remove(p);
            --p;
            if (pipeline->programs.len == 0)
                return;
        }
        if (*force_quit)
            return;
    }
}

///////////////////////////////////////////////////////////////////////////////

static void read_tty_output(Backlog_State* backlog, Pseudo_Terminal* tty, bool cap_read_calls) {
    static char buffer[4096];

#ifdef _WIN32
    cz::Input_File parent_out = tty->out;
#else
    cz::Input_File parent_out;
    parent_out.handle = tty->parent_bi;
#endif
    if (parent_out.is_open()) {
        int64_t result = 0;
        for (int rounds = 0;; ++rounds) {
            if (cap_read_calls && rounds == 1024)
                break;

            // Note: CRLF is stripped in append_text.
            result = parent_out.read(buffer, sizeof(buffer));
            if (result <= 0)
                break;

            // TODO: allow expanding max dynamically (don't close script->out here)
            result = append_text(backlog, {buffer, (size_t)result});
            if (result <= 0)
                break;
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
// Tick program
///////////////////////////////////////////////////////////////////////////////

static bool tick_program(Shell_State* shell,
                         Shell_Local* local,
                         Render_State* rend,
                         Prompt_State* prompt,
                         cz::Slice<Backlog_State*> backlogs,
                         Backlog_State* backlog,
                         cz::Allocator allocator,
                         Running_Program* program,
                         Pseudo_Terminal* tty,
                         int* exit_code,
                         bool* force_quit) {
    ZoneScoped;

    switch (program->type) {
    case Running_Program::PROCESS:
        return program->v.process.try_join(exit_code);

    case Running_Program::SUB: {
        Running_Node* node = &program->v.sub;
        while (1) {
            if (!tick_running_node(shell, backlogs, rend, prompt, node, tty, backlog, force_quit)) {
                break;  // TODO rate limit
            }
        }

        // TODO merge bg jobs up???
        if (node->fg_finished && node->bg.len == 0) {
            *exit_code = program->v.sub.fg.last_exit_code;
            cleanup_local(program->v.sub.local);
            cleanup_stdio(&program->v.sub.stdio);
            return true;
        }
    } break;

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
        while (st.file.file.is_open() || st.outer < builtin.args.len) {
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
            if (!st.file.file.is_open()) {
                cz::Str arg = builtin.args[st.outer];
                if (arg == "-") {
                    st.file = builtin.in;
                } else {
                    cz::String path = {};
                    cz::path::make_absolute(arg, get_wd(local), temp_allocator, &path);
                    st.file.polling = false;
                    if (!st.file.file.open(path.buffer)) {
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
                if (st.file.file.handle != builtin.in.file.handle)
                    st.file.file.close();
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
        (void)builtin.out.write(cz::format(temp_allocator, get_wd(local), '\n'));
        goto finish_builtin;
    } break;

    case Running_Program::CD: {
        auto& builtin = program->v.builtin;
        cz::String new_wd = {};
        cz::Str arg;
        if (builtin.args.len >= 2) {
            arg = builtin.args[1];
        } else if (!get_var(local, "HOME", &arg)) {
            builtin.exit_code = 1;
            (void)builtin.err.write("cd: No home directory.\n");
            goto finish_builtin;
        }
        standardize_arg(local, arg, temp_allocator, &new_wd, /*make_absolute=*/true);
        if (cz::file::is_directory(new_wd.buffer)) {
            set_wd(local, new_wd);
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
            int result = run_ls(builtin.out, &temp, get_wd(local), ".");
            if (result < 0) {
                builtin.exit_code = 1;
                (void)builtin.err.write("ls: error\n");
            }
        } else {
            for (size_t i = 1; i < builtin.args.len; ++i) {
                int result = run_ls(builtin.out, &temp, get_wd(local), builtin.args[i]);
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
                // TODO: not permanent but close...
                cz::Str script = cz::format(permanent_allocator, value, " \"$@\"");

                Shell_Node* node = permanent_allocator.alloc<Shell_Node>();
                *node = {};
                Error error = parse_script(permanent_allocator, node, script);

                // Try not appending the "$@" just in case the user
                // does 'alias x="if true; then echo hi; fi"'.
                if (error == Error_Parse_ExpectedEndOfStatement) {
                    error =
                        parse_script(permanent_allocator, node, script.slice_end(script.len - 5));
                }

                if (error != Error_Success) {
                    (void)builtin.err.write("alias: Error: ");
                    (void)builtin.err.write(error_string(error));
                    (void)builtin.err.write("\n");
                    continue;
                }

                set_alias(local, key, node);
            } else {
                size_t i = 0;
                for (; i < local->alias_names.len; ++i) {
                    if (arg == local->alias_names[i]) {
                        (void)builtin.out.write("alias: ");
                        (void)builtin.out.write(local->alias_names[i]);
                        (void)builtin.out.write(" is aliased to: ");
                        cz::String string = {};
                        append_node(temp_allocator, &string, local->alias_values[i], false);
                        (void)builtin.out.write(string);
                        string.drop(temp_allocator);
                        (void)builtin.out.write("\n");
                        break;
                    }
                }
                if (i == local->alias_names.len) {
                    builtin.exit_code = 1;
                    (void)builtin.err.write(
                        cz::format(temp_allocator, "alias: ", arg, ": unbound alias\n"));
                }
            }
        }
        goto finish_builtin;
    } break;

    case Running_Program::CONFIGURE: {
        auto& builtin = program->v.builtin;
        if (builtin.args.len != 3) {
            (void)builtin.err.write("configure: Usage: configure [option] [value]\n");
            goto finish_builtin;
        }

        cz::Str option = builtin.args[1];
        int value;
        if (!cz::parse(builtin.args[2], &value)) {
            (void)builtin.err.write("configure: Usage: configure [option] [value]\n");
            goto finish_builtin;
        }

        if (option == "font_size") {
            if (value <= 0) {
                (void)builtin.err.write("configure: Invalid font_height\n");
            } else {
                resize_font(value, rend);
            }
        } else {
            (void)builtin.err.write(
                cz::format(temp_allocator, "configure: Unrecognized option ", option, '\n'));
        }
        goto finish_builtin;
    }

    case Running_Program::VARIABLES: {
        auto& st = program->v.builtin.st.variables;
        for (size_t i = 0; i < st.names.len; ++i) {
            set_var(local, st.names[i], st.values[i]);
        }
        goto finish_builtin;
    } break;

    case Running_Program::WHICH: {
        auto& builtin = program->v.builtin;
        cz::String path = {};
        for (size_t i = 1; i < builtin.args.len; ++i) {
            cz::Str arg = builtin.args[i];
            path.len = 0;
            if (find_in_path(local, arg, temp_allocator, &path)) {
                path.push('\n');
                (void)builtin.out.write(path);
            } else {
                builtin.exit_code = 1;
                (void)builtin.err.write(
                    cz::format(temp_allocator, "which: Couldn't find ", arg, '\n'));
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
        for (size_t i = 1; i < builtin.args.len; ++i) {
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
                set_var(local, key, value);
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
        auto& st = program->v.builtin.st.source;

        // First time through start running.
        if (builtin.args.len <= 1) {
            builtin.exit_code = 1;
            (void)builtin.err.write("source: No file specified\n");
            goto finish_builtin;
        }

        cz::String path = {};
        cz::path::make_absolute(builtin.args[1], get_wd(local), temp_allocator, &path);
        cz::Input_File file;
        if (!file.open(path.buffer)) {
            builtin.exit_code = 1;
            (void)builtin.err.write(
                cz::format(temp_allocator, "source: Couldn't open file ", builtin.args[1], '\n'));
            goto finish_builtin;
        }
        CZ_DEFER(file.close());

        cz::String contents = {};
        read_to_string(file, allocator, &contents);

        // Root has to be kept alive for path traversal to work.
        Shell_Node* root = allocator.alloc<Shell_Node>();
        *root = {};

        Error error = parse_script(allocator, root, contents);
        if (error == Error_Success) {
            cz::Vector<cz::Str> args = builtin.args.slice_start(2).clone(allocator);
            Stdio_State stdio = st.stdio;

            // Convert this node to a sub node.
            program->type = Running_Program::SUB;
            Running_Node* node = &program->v.sub;
            *node = {};
            node->stdio = stdio;
            node->local = allocator.alloc<Shell_Local>();
            *node->local = {};
            node->local->parent = local;
            node->local->args = args;
            node->local->relationship = Shell_Local::ARGS_ONLY;

            error = start_execute_node(shell, *tty, backlog, node, root);
            if (error == Error_Success) {
                break;
            }
        }

        append_text(backlog, "source: Error: ");
        append_text(backlog, error_string(error));
        append_text(backlog, "\n");
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

    case Running_Program::ATTACH: {
        uint64_t this_process = backlog->id;
        rend->scroll_mode = AUTO_SCROLL;
        shell->selected_process = this_process;
        shell->attached_process = this_process;
        goto finish_builtin;
    } break;

    case Running_Program::FOLLOW: {
        uint64_t this_process = backlog->id;
        rend->scroll_mode = AUTO_SCROLL;
        shell->selected_process = this_process;
        goto finish_builtin;
    } break;

    case Running_Program::ARGDUMP: {
        auto& builtin = program->v.builtin;
        for (size_t i = 1; i < builtin.args.len; ++i) {
            (void)builtin.out.write(builtin.args[i]);
            (void)builtin.out.write("\n");
        }
        goto finish_builtin;
    } break;

    case Running_Program::VARDUMP: {
        auto& builtin = program->v.builtin;
        for (Shell_Local* iter = local; iter; iter = iter->parent) {
            if (iter != local)
                (void)builtin.out.write("\n");
            for (size_t i = 0; i < iter->variable_names.len; ++i) {
                (void)builtin.out.write(iter->variable_names[i]);
                (void)builtin.out.write("=");
                (void)builtin.out.write(iter->variable_values[i]);
                (void)builtin.out.write("\n");
            }
        }
        goto finish_builtin;
    } break;

    case Running_Program::SHIFT: {
        if (local->args.len > 0)
            local->args.remove(0);
        goto finish_builtin;
    } break;

    case Running_Program::HISTORY: {
        auto& builtin = program->v.builtin;
        auto& st = builtin.st.history;
        while (1) {
            if (st.outer == prompt->history.len)
                goto finish_builtin;

            cz::Str elem = prompt->history[st.outer];

            if (st.inner < elem.len) {
                cz::Str slice = elem.slice_start(st.inner);
                int64_t wrote = builtin.out.write(slice);
                if (wrote != slice.len) {
                    if (wrote == 0)
                        goto finish_builtin;
                    if (wrote > 0)
                        st.inner += wrote;
                    break;
                }
            }

            {
                int64_t wrote = builtin.out.write("\n");
                if (wrote != 1) {
                    if (wrote == 0)
                        goto finish_builtin;
                    st.inner = elem.len;
                    break;
                }
            }

            st.outer++;
            st.inner = 0;
        }
    } break;

    default:
        CZ_PANIC("unreachable");
    }
    return false;

finish_builtin:
    auto& builtin = program->v.builtin;
    *exit_code = builtin.exit_code;
    cleanup_builtin(program);
    return true;
}

///////////////////////////////////////////////////////////////////////////////

static void standardize_arg(const Shell_Local* local,
                            cz::Str arg,
                            cz::Allocator allocator,
                            cz::String* new_wd,
                            bool make_absolute) {
    if (make_absolute) {
        cz::path::make_absolute(arg, get_wd(local), allocator, new_wd);
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
