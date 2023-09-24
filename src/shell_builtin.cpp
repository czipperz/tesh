#include "shell.hpp"

#include <tracy/Tracy.hpp>
#include <cz/char_type.hpp>
#include <cz/defer.hpp>
#include <cz/directory.hpp>
#include <cz/format.hpp>
#include <cz/parse.hpp>
#include <cz/path.hpp>
#include "config.hpp"
#include "global.hpp"
#include "prompt.hpp"

////////////////////////////////////////////////////////////////////////////////
// Builtin levels
////////////////////////////////////////////////////////////////////////////////

static const Builtin level0[] = {
    {"exit", Builtin_Command::EXIT},
    {"return", Builtin_Command::RETURN},
    {"cd", Builtin_Command::CD},
    {"alias", Builtin_Command::ALIAS},
    {"function", Builtin_Command::FUNCTION},
    {"export", Builtin_Command::EXPORT},
    {"unset", Builtin_Command::UNSET},
    {"clear", Builtin_Command::CLEAR},
    {".", Builtin_Command::SOURCE},
    {"source", Builtin_Command::SOURCE},
    {"sleep", Builtin_Command::SLEEP},
    {"configure", Builtin_Command::CONFIGURE},
    {"attach", Builtin_Command::ATTACH},
    {"follow", Builtin_Command::FOLLOW},
    {"argdump", Builtin_Command::ARGDUMP},
    {"dump_arg", Builtin_Command::ARGDUMP},
    {"vardump", Builtin_Command::VARDUMP},
    {"dump_var", Builtin_Command::VARDUMP},
    {"funcdump", Builtin_Command::FUNCDUMP},
    {"dump_func", Builtin_Command::FUNCDUMP},
    {"aliasdump", Builtin_Command::ALIASDUMP},
    {"dump_alias", Builtin_Command::ALIASDUMP},
    {"shift", Builtin_Command::SHIFT},
    {"history", Builtin_Command::HISTORY},
    {"__tesh_set_var", Builtin_Command::SET_VAR},
    {"builtin", Builtin_Command::BUILTIN},
    {"mktemp", Builtin_Command::MKTEMP},
};

static const Builtin level1[] = {
    {"echo", Builtin_Command::ECHO},    {"pwd", Builtin_Command::PWD},
    {"which", Builtin_Command::WHICH},  {"true", Builtin_Command::TRUE_},
    {"false", Builtin_Command::FALSE_},
};

static const Builtin level2[] = {
    {"cat", Builtin_Command::CAT},
    {"ls", Builtin_Command::LS},
};

static const cz::Slice<const Builtin> the_builtin_levels[] = {level0, level1, level2};

const cz::Slice<const cz::Slice<const Builtin> > builtin_levels = the_builtin_levels;

////////////////////////////////////////////////////////////////////////////////
// Forward declarations
////////////////////////////////////////////////////////////////////////////////

static void standardize_arg(const Shell_Local* local,
                            cz::Str arg,
                            cz::Allocator allocator,
                            cz::String* new_wd,
                            bool make_absolute);

static int run_ls(Process_Output out,
                  cz::String* temp,
                  cz::Str working_directory,
                  cz::Str directory);

void clear_screen(Render_State* rend, Shell_State* shell, Prompt_State* prompt, bool in_script);

void load_history(Prompt_State* prompt, Shell_State* shell);
void save_history(Prompt_State* prompt, Shell_State* shell);

////////////////////////////////////////////////////////////////////////////////
// Recognize builtins
////////////////////////////////////////////////////////////////////////////////

void recognize_builtin(Running_Program* program, const Parse_Program& parse) {
    // Line that only assigns to variables runs as special builtin.
    //
    // Note: this can also be hit when evaluating `$()` or `$(;)` in which
    // case we still can use VARIABLES because it'll just do nothing.
    if (parse.v.args.len == 0) {
        program->type = Running_Program::ANY_BUILTIN;
        program->v.builtin.command = Builtin_Command::VARIABLES;
        program->v.builtin.st.variables = {};
        program->v.builtin.st.variables.names = parse.variable_names;
        program->v.builtin.st.variables.values = parse.variable_values;
        return;
    }

    for (size_t i = 0; i <= cfg.builtin_level; ++i) {
        cz::Slice<const Builtin> builtins = builtin_levels[i];
        for (size_t j = 0; j < builtins.len; ++j) {
            const Builtin& builtin = builtins[j];
            if (parse.v.args[0] == builtin.name) {
                program->type = Running_Program::ANY_BUILTIN;
                program->v.builtin.command = builtin.command;
                return;
            }
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// Setup builtin
////////////////////////////////////////////////////////////////////////////////

void setup_builtin(Running_Builtin* builtin, cz::Allocator allocator, Stdio_State stdio) {
    if (builtin->command == Builtin_Command::SOURCE) {
        builtin->st.source = {};
        builtin->st.source.stdio = stdio;
    } else if (builtin->command == Builtin_Command::SLEEP) {
        builtin->st.sleep = {};
        builtin->st.sleep.start = std::chrono::steady_clock::now();
    } else if (builtin->command == Builtin_Command::ECHO) {
        builtin->st.echo = {};
        builtin->st.echo.outer = 1;
    } else if (builtin->command == Builtin_Command::CAT) {
        builtin->st.cat = {};
        builtin->st.cat.buffer = (char*)allocator.alloc({4096, 1});
        builtin->st.cat.outer = 0;
    } else if (builtin->command == Builtin_Command::SET_VAR) {
        builtin->st.set_var = {};
    }
}

////////////////////////////////////////////////////////////////////////////////
// Tick builtin
////////////////////////////////////////////////////////////////////////////////

bool tick_builtin(Shell_State* shell,
                  Shell_Local* local,
                  Render_State* rend,
                  Prompt_State* prompt,
                  Backlog_State* backlog,
                  cz::Allocator allocator,
                  Running_Program* program,
                  Pseudo_Terminal* tty,
                  int* exit_code,
                  bool* force_quit) {
    ZoneScoped;

    Running_Builtin* builtin = &program->v.builtin;

    switch (builtin->command) {
    case Builtin_Command::INVALID: {
        auto& st = builtin->st.invalid;
        (void)builtin->err.write(cz::format(temp_allocator, "tesh: ", st.m1, ": ", st.m2, '\n'));
        builtin->exit_code = 1;
        goto finish_builtin;
    } break;

    case Builtin_Command::ECHO: {
        auto& st = builtin->st.echo;
        int64_t result = 0;
        int rounds = 0;
        for (; st.outer < builtin->args.len; ++st.outer) {
            // Rate limit to prevent hanging.
            if (rounds++ == 1024)
                return false;

            // Write this arg.
            cz::Str arg = builtin->args[st.outer];
            if (st.inner != arg.len) {
                result = builtin->out.write(arg.buffer + st.inner, arg.len - st.inner);
                if (result <= 0)
                    break;

                st.inner += result;
                if (st.inner != arg.len)
                    continue;
            }

            // Write a trailing space.
            if (st.outer + 1 < builtin->args.len) {
                result = builtin->out.write(" ", 1);
                if (result <= 0)
                    break;
                st.inner = 0;
            }
        }

        // Write a final newline.
        if (st.outer == builtin->args.len)
            result = builtin->out.write("\n");

        if (result >= 0) {
            // If completely done or found eof then stop.
            goto finish_builtin;
        }
    } break;

    case Builtin_Command::CAT: {
        auto& st = builtin->st.cat;

        if (st.outer == 0) {
            ++st.outer;
            if (builtin->args.len == 1) {
                st.file = builtin->in;
            }
        }

        int64_t result = 0;
        int rounds = 0;
        while (st.file.file.is_open() || st.outer < builtin->args.len) {
            // Rate limit to prevent hanging.
            if (rounds++ == 1024)
                return false;

            // Write remaining buffer.
            if (st.offset != st.len) {
                result = builtin->out.write(st.buffer + st.offset, st.len - st.offset);
                if (result <= 0)
                    break;

                st.offset += result;
                if (st.offset != st.len)
                    continue;
            }

            // Get a new file if we don't have one.
            if (!st.file.file.is_open()) {
                cz::Str arg = builtin->args[st.outer];
                if (arg == "-") {
                    st.file = builtin->in;
                } else {
                    cz::String path = {};
                    cz::path::make_absolute(arg, get_wd(local), temp_allocator, &path);
                    st.file.polling = false;
                    if (!st.file.file.open(path.buffer)) {
                        builtin->exit_code = 1;
                        cz::Str message = cz::format("cat: ", arg, ": No such file or directory\n");
                        (void)builtin->err.write(message);
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
                if (st.file.file.handle != builtin->in.file.handle)
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

    case Builtin_Command::EXIT:
    case Builtin_Command::RETURN: {
        if (builtin->args.len == 1) {
            builtin->exit_code = 0;
        } else {
            builtin->exit_code = 1;
            if (!cz::parse(builtin->args[1], &builtin->exit_code)) {
                if (builtin->command == Builtin_Command::EXIT)
                    (void)builtin->err.write("exit: Invalid code\n");
                else
                    (void)builtin->err.write("return: Invalid code\n");
            }
        }
        if (builtin->command == Builtin_Command::EXIT)
            *force_quit = true;
        goto finish_builtin;
    } break;

    case Builtin_Command::PWD: {
        (void)builtin->out.write(cz::format(temp_allocator, get_wd(local), '\n'));
        goto finish_builtin;
    } break;

    case Builtin_Command::CD: {
        cz::String new_wd = {};
        cz::Str arg;
        if (builtin->args.len >= 2) {
            arg = builtin->args[1];
        } else if (!get_var(local, "HOME", &arg)) {
            builtin->exit_code = 1;
            (void)builtin->err.write("cd: No home directory.\n");
            goto finish_builtin;
        }
        standardize_arg(local, arg, temp_allocator, &new_wd, /*make_absolute=*/true);
        if (cz::file::is_directory(new_wd.buffer)) {
            set_wd(local, new_wd);
        } else {
            if (builtin->args.len >= 2 && arg.starts_with('-')) {
                uint64_t num = 0;
                if (arg.len == 1) {
                    num = 1;
                } else {
                    (void)cz::parse(arg.slice_start(1), &num);
                }

                if (num != 0) {
                    cz::Str new_wd2;
                    if (get_old_wd(local, num, &new_wd2)) {
                        if (cz::file::is_directory(new_wd2.buffer)) {
                            set_wd(local, new_wd2);
                            goto finish_builtin;
                        }
                    }
                }
            }

            builtin->exit_code = 1;
            (void)builtin->err.write("cd: ");
            (void)builtin->err.write(new_wd.buffer, new_wd.len);
            (void)builtin->err.write(": Not a directory\n");
        }
        goto finish_builtin;
    } break;

    case Builtin_Command::LS: {
        cz::String temp = {};
        CZ_DEFER(temp.drop(temp_allocator));
        if (builtin->args.len == 1) {
            int result = run_ls(builtin->out, &temp, get_wd(local), ".");
            if (result < 0) {
                builtin->exit_code = 1;
                (void)builtin->err.write("ls: error\n");
            }
        } else {
            for (size_t i = 1; i < builtin->args.len; ++i) {
                int result = run_ls(builtin->out, &temp, get_wd(local), builtin->args[i]);
                if (result < 0) {
                    builtin->exit_code = 1;
                    (void)builtin->err.write("ls: error\n");
                    break;
                }
            }
        }
        goto finish_builtin;
    } break;

    case Builtin_Command::ALIAS: {
        for (size_t i = 1; i < builtin->args.len; ++i) {
            cz::Str arg = builtin->args[i];
            if (arg.len == 0 || arg[0] == '=') {
                builtin->exit_code = 1;
                (void)builtin->err.write("alias: ");
                (void)builtin->err.write(arg);
                (void)builtin->err.write(": invalid alias name\n");
                continue;
            }

            cz::Str key, value;
            if (arg.split_excluding('=', &key, &value)) {
                // TODO: not permanent but close...
                cz::Str script = cz::format(permanent_allocator, value, " \"$@\"");

                Parse_Node* node = permanent_allocator.alloc<Parse_Node>();
                *node = {};
                Error error = parse_script(permanent_allocator, node, script);

                // Try not appending the "$@" just in case the user
                // does 'alias x="if true; then echo hi; fi"'.
                if (error == Error_Parse_ExpectedEndOfStatement) {
                    error =
                        parse_script(permanent_allocator, node, script.slice_end(script.len - 5));
                }

                if (error != Error_Success) {
                    (void)builtin->err.write("alias: Error: ");
                    (void)builtin->err.write(error_string(error));
                    (void)builtin->err.write("\n");
                    continue;
                }

                set_alias(local, key, node);
            } else {
                Parse_Node* alias = get_alias_no_recursion_check(local, arg);
                if (alias) {
                    (void)builtin->out.write("alias: ");
                    (void)builtin->out.write(arg);
                    (void)builtin->out.write(" is aliased to: ");
                    cz::String string = {};
                    append_parse_node(temp_allocator, &string, alias, false);
                    (void)builtin->out.write(string);
                    string.drop(temp_allocator);
                    (void)builtin->out.write("\n");
                    break;
                } else {
                    builtin->exit_code = 1;
                    (void)builtin->err.write(
                        cz::format(temp_allocator, "alias: ", arg, ": unbound alias\n"));
                }
            }
        }
        goto finish_builtin;
    } break;

    case Builtin_Command::FUNCTION: {
        for (size_t i = 1; i < builtin->args.len; ++i) {
            cz::Str arg = builtin->args[i];
            Parse_Node* func = get_function(local, arg);
            if (func) {
                (void)builtin->out.write("function: ");
                (void)builtin->out.write(arg);
                (void)builtin->out.write(" is defined as: ");
                cz::String string = {};
                append_parse_node(temp_allocator, &string, func, false);
                (void)builtin->out.write(string);
                string.drop(temp_allocator);
                (void)builtin->out.write("\n");
            } else {
                builtin->exit_code = 1;
                (void)builtin->err.write(
                    cz::format(temp_allocator, "function: ", arg, ": undefined function\n"));
            }
        }
        goto finish_builtin;
    } break;

    case Builtin_Command::CONFIGURE: {
        if (builtin->args.len != 3) {
            (void)builtin->err.write(
                "configure: Usage: configure [option] [value]\n\
\n\
Options:\n\
history_file   PATH  -- Reload command history.\n\
font_path      PATH  -- Set the font\n\
font_size      SIZE  -- Set the font size.\n\
builtin_level  LEVEL -- Set the builtin level (see builtin --help).\n\
wide_terminal  1/0   -- Turn on or off wide terminal mode.  This will lock the terminal's width\n\
                        at 1000 characters instead of the actual width.\n\
");
            goto finish_builtin;
        }

        cz::Str option = builtin->args[1];

        if (option == "history_file") {
            prompt->history_path.drop(cz::heap_allocator());
            prompt->history_path = builtin->args[2].clone_null_terminate(cz::heap_allocator());
            load_history(prompt, shell);
            goto finish_builtin;
        } else if (option == "font_path") {
            cfg.font_path.drop(cz::heap_allocator());
            cfg.font_path = builtin->args[2].clone_null_terminate(cz::heap_allocator());
            resize_font(rend->font_size, rend);
            goto finish_builtin;
        }

        int value;
        if (!cz::parse(builtin->args[2], &value)) {
            (void)builtin->err.write("configure: Usage: configure [option] [value]\n");
            goto finish_builtin;
        }

        if (option == "font_size") {
            if (value <= 0) {
                (void)builtin->err.write("configure: Invalid font size.\n");
            } else {
                resize_font(value, rend);
            }
        } else if (option == "builtin_level") {
            if (value < 0 || value > 2) {
                (void)builtin->err.write("configure: Invalid builtin level.\n");
            } else {
                cfg.builtin_level = value;
            }
        } else if (option == "wide_terminal") {
            if (value < 0 || value > 1) {
                (void)builtin->err.write("configure: Invalid boolean value.\n");
            } else {
                cfg.windows_wide_terminal = value;
            }
        } else {
            (void)builtin->err.write(
                cz::format(temp_allocator, "configure: Unrecognized option ", option, '\n'));
        }
        goto finish_builtin;
    }

    case Builtin_Command::VARIABLES: {
        auto& st = builtin->st.variables;
        for (size_t i = 0; i < st.names.len; ++i) {
            set_var(local, st.names[i], st.values[i]);
        }
        goto finish_builtin;
    } break;

    case Builtin_Command::WHICH: {
        cz::String path = {};
        for (size_t i = 1; i < builtin->args.len; ++i) {
            cz::Str arg = builtin->args[i];
            path.len = 0;
            if (find_in_path(local, arg, temp_allocator, &path)) {
                path.push('\n');
                (void)builtin->out.write(path);
            } else {
                builtin->exit_code = 1;
                (void)builtin->err.write(
                    cz::format(temp_allocator, "which: Couldn't find ", arg, '\n'));
            }
        }
        goto finish_builtin;
    } break;

    case Builtin_Command::TRUE_:
        builtin->exit_code = 0;
        goto finish_builtin;
    case Builtin_Command::FALSE_:
        builtin->exit_code = 1;
        goto finish_builtin;

    case Builtin_Command::EXPORT: {
        for (size_t i = 1; i < builtin->args.len; ++i) {
            cz::Str arg = builtin->args[i];
            cz::Str key = arg, value;
            if (arg.split_excluding('=', &key, &value)) {
                if (key.len == 0) {
                    builtin->exit_code = 1;
                    (void)builtin->err.write("export: Empty variable name: ");
                    (void)builtin->err.write(arg);
                    (void)builtin->err.write("\n");
                    continue;
                }
                for (size_t j = 0; j < key.len; ++j) {
                    if (!cz::is_alnum(key[j]) && key[j] != '_') {
                        builtin->exit_code = 1;
                        (void)builtin->err.write("export: Invalid variable name: ");
                        (void)builtin->err.write(arg);
                        (void)builtin->err.write("\n");
                        goto next;
                    }
                }
                set_var(local, key, value);
            }
            make_env_var(local, key);
        next:;
        }
        goto finish_builtin;
    } break;

    case Builtin_Command::UNSET: {
        for (size_t i = 1; i < builtin->args.len; ++i) {
            cz::Str key = builtin->args[i];
            unset_var(local, key);
        }
        goto finish_builtin;
    } break;

    case Builtin_Command::CLEAR: {
        clear_screen(rend, shell, prompt, true);
        goto finish_builtin;
    } break;

    case Builtin_Command::SOURCE: {
        auto& st = builtin->st.source;

        // First time through start running.
        if (builtin->args.len <= 1) {
            builtin->exit_code = 1;
            (void)builtin->err.write("source: No file specified\n");
            goto finish_builtin;
        }

        cz::String path = {};
        cz::path::make_absolute(builtin->args[1], get_wd(local), temp_allocator, &path);
        cz::Input_File file;
        if (!file.open(path.buffer)) {
            builtin->exit_code = 1;
            (void)builtin->err.write(
                cz::format(temp_allocator, "source: Couldn't open file ", builtin->args[1], '\n'));
            goto finish_builtin;
        }
        CZ_DEFER(file.close());

        cz::String contents = {};
        read_to_string(file, allocator, &contents);

        // Root has to be kept alive for path traversal to work.
        Parse_Node* root = allocator.alloc<Parse_Node>();
        *root = {};

        Error error = parse_script(allocator, root, contents);
        if (error == Error_Success) {
            cz::Vector<cz::Str> args = builtin->args.slice_start(2).clone(allocator);
            Stdio_State stdio = st.stdio;

            // Canibalize this node into the script to be ran (ala
            // execve) by converting `program` to a sub node.
            // Note: Because this is `source` we don't use COW mode.
            program->type = Running_Program::SUB;
            program->v.sub = build_sub_running_node(local, stdio, allocator);
            program->v.sub.local->args = args;

            error = start_execute_node(shell, *tty, backlog, &program->v.sub, root);
            if (error == Error_Success) {
                break;
            }
        }

        builtin->err.write(
            cz::format(temp_allocator, "source: Error: ", error_string(error), "\n"));
        goto finish_builtin;
    } break;

    case Builtin_Command::SLEEP: {
        auto& st = builtin->st.sleep;

        if (builtin->args.len <= 1) {
            builtin->exit_code = 1;
            (void)builtin->err.write("sleep: No time specified\n");
            goto finish_builtin;
        }

        uint64_t max = 0;
        if (!cz::parse(builtin->args[1], &max)) {
            builtin->exit_code = 1;
            (void)builtin->err.write(cz::format(
                temp_allocator, "sleep: Invalid time specified: ", builtin->args[1], "\n"));
            goto finish_builtin;
        }

        auto now = std::chrono::steady_clock::now();
        uint64_t actual = std::chrono::duration_cast<std::chrono::seconds>(now - st.start).count();
        if (actual >= max)
            goto finish_builtin;
    } break;

    case Builtin_Command::ATTACH: {
        uint64_t this_process = backlog->id;
        rend->scroll_mode = AUTO_SCROLL;

        // Reorder the attached process to be last.
        size_t visindex = find_visbacklog(rend, this_process);
        if (visindex != -1) {
            CZ_DEBUG_ASSERT(rend->visbacklogs[visindex]->id == backlog->id);
            rend->attached_outer = rend->visbacklogs.len - 1;
            reorder_attached_to_last(rend);
            prompt->history_counter = prompt->stdin_history.len;
        }
        goto finish_builtin;
    } break;

    case Builtin_Command::FOLLOW: {
        uint64_t this_process = backlog->id;
        rend->scroll_mode = AUTO_SCROLL;

        size_t visindex = find_visbacklog(rend, this_process);
        if (visindex != -1)
            rend->selected_outer = visindex;
        goto finish_builtin;
    } break;

    case Builtin_Command::ARGDUMP: {
        for (size_t i = 1; i < builtin->args.len; ++i) {
            (void)builtin->out.write(builtin->args[i]);
            (void)builtin->out.write("\n");
        }
        goto finish_builtin;
    } break;

    case Builtin_Command::ALIASDUMP: {
        for (Shell_Local* iter = local; iter; iter = iter->parent) {
            if (iter != local)
                (void)builtin->out.write("\n");
            for (size_t i = 0; i < iter->alias_names.len; ++i) {
                (void)builtin->out.write("alias: ");
                (void)builtin->out.write(iter->alias_names[i]);
                (void)builtin->out.write(" is aliased to: ");
                cz::String string = {};
                append_parse_node(temp_allocator, &string, iter->alias_values[i],
                                  /*append_semicolon=*/false);
                (void)builtin->out.write(string);
                string.drop(temp_allocator);
                (void)builtin->out.write("\n");
            }
        }
        goto finish_builtin;
    } break;

    case Builtin_Command::FUNCDUMP: {
        for (Shell_Local* iter = local; iter; iter = iter->parent) {
            if (iter != local)
                (void)builtin->out.write("\n");
            for (size_t i = 0; i < iter->function_names.len; ++i) {
                (void)builtin->out.write("function: ");
                (void)builtin->out.write(iter->function_names[i]);
                (void)builtin->out.write(" is defined as: ");
                cz::String string = {};
                append_parse_node(temp_allocator, &string, iter->function_values[i],
                                  /*append_semicolon=*/false);
                (void)builtin->out.write(string);
                string.drop(temp_allocator);
                (void)builtin->out.write("\n");
            }
        }
        goto finish_builtin;
    } break;

    case Builtin_Command::VARDUMP: {
        for (Shell_Local* iter = local; iter; iter = iter->parent) {
            if (iter != local)
                (void)builtin->out.write("\n");
            for (size_t i = 0; i < iter->variable_names.len; ++i) {
                (void)builtin->out.write(iter->variable_names[i].str);
                (void)builtin->out.write("=");
                (void)builtin->out.write(iter->variable_values[i].str);
                (void)builtin->out.write("\n");
            }
        }
        goto finish_builtin;
    } break;

    case Builtin_Command::SHIFT: {
        if (local->args.len > 0)
            local->args.remove(0);
        goto finish_builtin;
    } break;

    case Builtin_Command::HISTORY: {
        auto& st = builtin->st.history;
        while (1) {
            if (st.outer == prompt->history.len)
                goto finish_builtin;

            cz::Str elem = prompt->history[st.outer];

            if (st.inner < elem.len) {
                cz::Str slice = elem.slice_start(st.inner);
                int64_t wrote = builtin->out.write(slice);
                if (wrote != slice.len) {
                    if (wrote == 0)
                        goto finish_builtin;
                    if (wrote > 0)
                        st.inner += wrote;
                    break;
                }
            }

            {
                int64_t wrote = builtin->out.write("\n");
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

    case Builtin_Command::SET_VAR: {
        auto& st = builtin->st.set_var;

        if (builtin->args.len != 2) {
            goto finish_builtin;
        }

        for (int rounds = 0; rounds < 128; ++rounds) {
            char buffer[4096];
            int64_t result = builtin->in.read(buffer, sizeof(buffer));
            if (result > 0) {
                st.value.reserve(cz::heap_allocator(), result);
                st.value.append({buffer, (size_t)result});
            } else if (result == 0) {
                if (st.value.ends_with('\n'))
                    st.value.len--;  // Remove trailing newline.
                set_var(local, builtin->args[1], st.value);
                st.value.drop(cz::heap_allocator());
                goto finish_builtin;
            } else {
                break;
            }
        }
    } break;

    case Builtin_Command::BUILTIN: {
        if (builtin->args.len == 1 || (builtin->args.len == 2 && builtin->args[1] == "--help")) {
            for (size_t level = 0; level <= cfg.builtin_level; ++level) {
                cz::Slice<const Builtin> builtins = builtin_levels[level];
                for (size_t j = 0; j < builtins.len; ++j) {
                    const Builtin& the_builtin = builtins[j];
                    builtin->out.write(cz::format(temp_allocator, the_builtin.name, '\n'));
                }
            }
            goto finish_builtin;
        }

        for (size_t i = 1; i < builtin->args.len; ++i) {
            cz::Str arg = builtin->args[i];
            for (size_t level = 0; level <= cfg.builtin_level; ++level) {
                cz::Slice<const Builtin> builtins = builtin_levels[level];
                for (size_t j = 0; j < builtins.len; ++j) {
                    const Builtin& the_builtin = builtins[j];
                    if (arg == the_builtin.name) {
                        (void)builtin->out.write(cz::format(temp_allocator, arg, '\n'));
                        goto continue_outer;
                    }
                }
            }

            builtin->exit_code = 1;
            (void)builtin->err.write(
                cz::format(temp_allocator, "builtin: Couldn't find ", arg, '\n'));

        continue_outer:;
        }
        goto finish_builtin;
    } break;

    case Builtin_Command::MKTEMP: {
        char temp_file_buffer[L_tmpnam];
        if (tmpnam(temp_file_buffer)) {
            (void)builtin->out.write(temp_file_buffer);
        } else {
            (void)builtin->err.write("mktemp: Failed to create temp file\n");
        }

        goto finish_builtin;
    } break;

    default:
        CZ_PANIC("unreachable");
    }

    return false;

finish_builtin:
    *exit_code = builtin->exit_code;
    cleanup_builtin(program);
    return true;
}

////////////////////////////////////////////////////////////////////////////////
// Utility functions
////////////////////////////////////////////////////////////////////////////////

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
