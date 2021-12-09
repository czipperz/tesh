#include "shell.hpp"

#include <Tracy.hpp>
#include <cz/defer.hpp>
#include <cz/heap.hpp>
#include <cz/path.hpp>
#include <cz/process.hpp>

#include "config.hpp"
#include "global.hpp"

///////////////////////////////////////////////////////////////////////////////
// Forward declarationss
///////////////////////////////////////////////////////////////////////////////

static Error start_execute_node(Shell_State* shell,
                                const Pseudo_Terminal& tty,
                                Backlog_State* backlog,
                                Running_Node* node,
                                Shell_Node* root);

static Error start_execute_line(Shell_State* shell,
                                const Pseudo_Terminal& tty,
                                Running_Node* node,
                                Backlog_State* backlog,
                                Running_Pipeline* pipeline,
                                bool background);

static Error start_execute_pipeline(Shell_State* shell,
                                    const Pseudo_Terminal& tty,
                                    Running_Node* node,
                                    Backlog_State* backlog,
                                    Running_Pipeline* pipeline,
                                    bool bind_stdin);

static Error run_program(Shell_State* shell,
                         Shell_Local* local,
                         cz::Allocator allocator,
                         const Pseudo_Terminal& tty,
                         Running_Program* program,
                         Parse_Program parse,
                         Stdio_State stdio,
                         Backlog_State* backlog);

static void recognize_builtins(Running_Program* program,
                               const Parse_Program& parse,
                               cz::Allocator allocator);

///////////////////////////////////////////////////////////////////////////////

enum Walk_Status {
    WALK_FAILURE = 0,
    WALK_SUCCESS = 1,
    WALK_ASYNC = 2,
};

static bool descend_to_first_pipeline(cz::Vector<Shell_Node*>* path, Shell_Node* child);
static void do_descend_to_first_pipeline(cz::Vector<Shell_Node*>* path, Shell_Node* child);
static bool walk_to_next_pipeline(cz::Vector<Shell_Node*>* path, Walk_Status status);

///////////////////////////////////////////////////////////////////////////////
// Start executing a script
///////////////////////////////////////////////////////////////////////////////

Error start_execute_script(Shell_State* shell,
                           Backlog_State* backlog,
                           cz::Buffer_Array arena,
                           Shell_Node* root) {
    Running_Script running = {};
    running.id = backlog->id;
    running.arena = arena;

    if (!create_pseudo_terminal(&running.tty, shell->width, shell->height))
        return Error_IO;

    running.root.local = &shell->local;

#ifdef _WIN32
    running.root.stdio.in = running.tty.child_in;
    running.root.stdio.out = running.tty.child_out;
#else
    running.root.stdio.in.handle = running.tty.child_bi;
    running.root.stdio.out.handle = running.tty.child_bi;
#endif
    running.root.stdio.err = running.root.stdio.out;  // stderr = stdout at top level

    Error error = start_execute_node(shell, running.tty, backlog, &running.root, root);
    if (error != Error_Success) {
        destroy_pseudo_terminal(&running.tty);
        return error;
    }

    shell->scripts.reserve(cz::heap_allocator(), 1);
    shell->scripts.push(running);

    if (cfg.on_spawn_attach) {
        shell->attached_process = running.id;
        shell->selected_process = running.id;
    }

    return Error_Success;
}

static Error start_execute_node(Shell_State* shell,
                                const Pseudo_Terminal& tty,
                                Backlog_State* backlog,
                                Running_Node* node,
                                Shell_Node* root) {
    node->fg.arena = alloc_arena(shell);

    if (!descend_to_first_pipeline(&node->fg.path, root))
        return Error_Success;

    const bool background = false;
    Error error = start_execute_line(shell, tty, node, backlog, &node->fg, background);
    if (error != Error_Success) {
        recycle_arena(shell, node->fg.arena);
    }
    return error;
}

///////////////////////////////////////////////////////////////////////////////
// Finish a line in the script
///////////////////////////////////////////////////////////////////////////////

bool finish_line(Shell_State* shell,
                 const Pseudo_Terminal& tty,
                 Running_Node* node,
                 Backlog_State* backlog,
                 Running_Pipeline* line,
                 bool background) {
#if defined(TRACY_ENABLE) && 0
    {
        cz::String message = cz::format(temp_allocator, "End: ", line->pipeline.command_line);
        TracyMessage(message.buffer, message.len);
    }
#endif

    Walk_Status status = (line->last_exit_code == 0 ? WALK_SUCCESS : WALK_FAILURE);
    bool has_next = walk_to_next_pipeline(&line->path, status);
    if (!has_next) {
        if (!background)
            backlog->exit_code = line->last_exit_code;
        recycle_pipeline(shell, line);
        if (background) {
            node->bg.remove(line - node->bg.elems);
        } else {
            node->fg_finished = true;
        }
        return false;
    }

    cleanup_pipeline(line);
    Error error = start_execute_line(shell, tty, node, backlog, line, background);
    if (error != Error_Success) {
        append_text(backlog, "Error: failed to execute continuation\n");
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// Path walking
///////////////////////////////////////////////////////////////////////////////

static bool descend_to_first_pipeline(cz::Vector<Shell_Node*>* path, Shell_Node* child) {
    do_descend_to_first_pipeline(path, child);
    if (path->last() != nullptr)
        return true;

    path->pop();
    return walk_to_next_pipeline(path, WALK_SUCCESS);
}

static void do_descend_to_first_pipeline(cz::Vector<Shell_Node*>* path, Shell_Node* child) {
    while (1) {
        path->reserve(cz::heap_allocator(), 1);
        path->push(child);

        switch (child->type) {
        case Shell_Node::PROGRAM:
        case Shell_Node::PIPELINE:
        case Shell_Node::FUNCTION:
            return;
        case Shell_Node::SEQUENCE:
            if (child->v.sequence.len == 0) {
                path->reserve(cz::heap_allocator(), 1);
                path->push(nullptr);
                return;
            } else {
                child = &child->v.sequence[0];
            }
            break;
        case Shell_Node::AND:
        case Shell_Node::OR:
            child = child->v.binary.left;
            break;
        case Shell_Node::IF:
            child = child->v.if_.cond;
            break;
        default:
            CZ_PANIC("Invalid Shell_Node type");
        }
    }
}

static bool walk_to_next_pipeline(cz::Vector<Shell_Node*>* path, Walk_Status status) {
    if (status == WALK_ASYNC)
        CZ_DEBUG_ASSERT(path->len > 0 && path->last()->async);
    bool success = (status != WALK_FAILURE);

    while (1) {
        if (path->len < 2) {
            path->len = 0;
            return false;
        }

        Shell_Node* child = path->pop();
        if (child->async) {
            if (status == WALK_ASYNC) {
                status = WALK_SUCCESS;
            } else {
                return false;
            }
        }

        Shell_Node* parent = path->last();
        switch (parent->type) {
        case Shell_Node::SEQUENCE: {
            size_t i = 0;
            for (; i < parent->v.sequence.len; ++i) {
                if (child == &parent->v.sequence[i]) {
                    ++i;
                    break;
                }
            }
            if (i < parent->v.sequence.len)
                return descend_to_first_pipeline(path, &parent->v.sequence[i]);
        } break;

        case Shell_Node::AND: {
            if (child == parent->v.binary.left && success)
                return descend_to_first_pipeline(path, parent->v.binary.right);
        } break;

        case Shell_Node::OR: {
            if (child == parent->v.binary.left && !success)
                return descend_to_first_pipeline(path, parent->v.binary.right);
        } break;

        case Shell_Node::IF:
            if (child == parent->v.if_.cond) {
                if (success)
                    return descend_to_first_pipeline(path, parent->v.if_.then);
                else if (parent->v.if_.other)
                    return descend_to_first_pipeline(path, parent->v.if_.other);
            }
            break;

        case Shell_Node::PROGRAM:
        case Shell_Node::PIPELINE:
        case Shell_Node::FUNCTION:
            CZ_PANIC("invalid");

        default:
            CZ_PANIC("Invalid Shell_Node type");
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
// Start execute line
///////////////////////////////////////////////////////////////////////////////

static Error start_execute_line(Shell_State* shell,
                                const Pseudo_Terminal& tty,
                                Running_Node* node,
                                Backlog_State* backlog,
                                Running_Pipeline* pipeline,
                                bool background) {
    if (backlog->length > 0 && backlog->get(backlog->length - 1) != '\n') {
        append_text(backlog, "\n");
    }

    Running_Pipeline* pipeline_orig = pipeline;
    while (1) {
        pipeline = pipeline_orig;
        bool async = pipeline->path.last()->async;
        if (async) {
            Running_Pipeline line = {};
            line.arena = alloc_arena(shell);
            line.path = pipeline->path.clone(cz::heap_allocator());
            node->bg.reserve(cz::heap_allocator(), 1);
            node->bg.push(line);
            pipeline = &node->bg.last();
        }

        bool bind_stdin = !(background || async);
        Error error = start_execute_pipeline(shell, tty, node, backlog, pipeline, bind_stdin);
        if (error != Error_Success)
            return error;

        if (!async) {
            return Error_Success;
        }

        // Find the next node to execute.
        if (!walk_to_next_pipeline(&pipeline_orig->path, WALK_ASYNC)) {
            // No node so stop.
            recycle_pipeline(shell, pipeline_orig);
            if (background) {
                node->bg.remove(pipeline_orig - node->bg.elems);
            } else {
                node->fg_finished = true;
            }
            return Error_Success;
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
// Start execute pipeline
///////////////////////////////////////////////////////////////////////////////

static Error start_execute_pipeline(Shell_State* shell,
                                    const Pseudo_Terminal& tty,
                                    Running_Node* node,
                                    Backlog_State* backlog,
                                    Running_Pipeline* pipeline,
                                    bool bind_stdin) {
    ZoneScoped;

    // TODO async stuff inside our children needs to be allocated separately.
    pipeline->arena.clear();

    // Get the nodes in the pipeline.
    Shell_Node* pipeline_node = pipeline->path.last();
    cz::Slice<Shell_Node> program_nodes;
    if (pipeline_node->type == Shell_Node::PIPELINE) {
        program_nodes = pipeline_node->v.pipeline;
    } else {
        // Only one element in the pipeline so it's left raw.
        program_nodes = {pipeline_node, 1};
    }

    cz::Allocator allocator = pipeline->arena.allocator();

    cz::Vector<Running_Program> programs = {};
    CZ_DEFER(programs.drop(cz::heap_allocator()));
    cz::Input_File pipe_in;

    for (size_t p = 0; p < program_nodes.len; ++p) {
        Shell_Node* program_node = &program_nodes[p];

        if (program_node->type == Shell_Node::FUNCTION) {
            // Declare the function and ignore all pipe and file indirection.
            set_function(node->local, program_node->v.function.name, program_node->v.function.body);
            continue;
        }

        CZ_ASSERT(program_node->type == Shell_Node::PROGRAM);  // TODO
        Parse_Program parse_program = *program_node->v.program;

        Stdio_State stdio = {};

        // Expand file arguments.
        if (parse_program.in_file.buffer) {
            cz::String file = {};
            expand_arg_single(node->local, parse_program.in_file, allocator, &file);
            // Reallocate to ensure the file isn't null and also don't waste space.
            file.realloc_null_terminate(allocator);
            parse_program.in_file = file;
        }
        if (parse_program.out_file.buffer) {
            cz::String file = {};
            expand_arg_single(node->local, parse_program.out_file, allocator, &file);
            // Reallocate to ensure the file isn't null and also don't waste space.
            file.realloc_null_terminate(allocator);
            parse_program.out_file = file;
        }
        if (parse_program.err_file.buffer) {
            cz::String file = {};
            expand_arg_single(node->local, parse_program.err_file, allocator, &file);
            // Reallocate to ensure the file isn't null and also don't waste space.
            file.realloc_null_terminate(allocator);
            parse_program.err_file = file;
        }

        cz::String path = {};
        if (parse_program.in_file.buffer) {
            stdio.in_type = File_Type_File;
            path.len = 0;
            cz::path::make_absolute(parse_program.in_file, get_wd(node->local), temp_allocator,
                                    &path);
            if (!stdio.in.open(path.buffer))
                return Error_InvalidPath;
            stdio.in_count = allocator.alloc<size_t>();
            *stdio.in_count = 1;
        } else if (p > 0) {
            stdio.in_type = File_Type_Pipe;
            stdio.in = pipe_in;
            stdio.in_count = allocator.alloc<size_t>();
            *stdio.in_count = 1;
        } else if (!bind_stdin) {
            stdio.in_type = File_Type_None;
        }
        pipe_in = {};

        if (parse_program.out_file.buffer) {
            stdio.out_type = File_Type_File;
            path.len = 0;
            cz::path::make_absolute(parse_program.out_file, get_wd(node->local), temp_allocator,
                                    &path);
            if (!stdio.out.open(path.buffer))
                return Error_InvalidPath;
            path.len = 0;
            stdio.out_count = allocator.alloc<size_t>();
            *stdio.out_count = 1;
        } else if (p + 1 < program_nodes.len) {
            stdio.out_type = File_Type_Pipe;
        }

        if (parse_program.err_file.buffer) {
            stdio.err_type = File_Type_File;
            path.len = 0;
            cz::path::make_absolute(parse_program.err_file, get_wd(node->local), temp_allocator,
                                    &path);
            if (!stdio.err.open(path.buffer))
                return Error_InvalidPath;
            path.len = 0;
            stdio.err_count = allocator.alloc<size_t>();
            *stdio.err_count = 1;
        }

        // Make pipes for the next iteration.
        if ((stdio.out_type == File_Type_Pipe || stdio.err_type == File_Type_Pipe) &&
            (p + 1 < program_nodes.len)) {
            Shell_Node* next = &program_nodes[p + 1];
            CZ_ASSERT(next->type == Shell_Node::PROGRAM);  // TODO
            if (!next->v.program->in_file.buffer) {
                cz::Output_File pipe_out;
                if (!cz::create_pipe(&pipe_in, &pipe_out))
                    return Error_IO;

                size_t* count = allocator.alloc<size_t>();
                *count = 0;
                if (stdio.out_type == File_Type_Pipe) {
                    stdio.out = pipe_out;
                    stdio.out_count = count;
                    ++*count;
                }
                if (stdio.err_type == File_Type_Pipe) {
                    stdio.err = pipe_out;
                    stdio.err_count = count;
                    ++*count;
                }
            }
        }

        Running_Program running_program = {};

        Error error = run_program(shell, node->local, allocator, tty, &running_program,
                                  parse_program, stdio, backlog);
        if (error != Error_Success)
            return error;

        programs.reserve(cz::heap_allocator(), 1);
        programs.push(running_program);
    }

    pipeline->programs = programs.clone(allocator);
    return Error_Success;
}

///////////////////////////////////////////////////////////////////////////////
// Run program
///////////////////////////////////////////////////////////////////////////////

static void generate_environment(void* out,
                                 const Shell_State* shell,
                                 const Shell_Local* local,
                                 cz::Slice<const cz::Str> variable_names,
                                 cz::Slice<const cz::Str> variable_values);

static Error run_program(Shell_State* shell,
                         Shell_Local* local,
                         cz::Allocator allocator,
                         const Pseudo_Terminal& tty,
                         Running_Program* program,
                         Parse_Program parse,
                         Stdio_State stdio,
                         Backlog_State* backlog) {
    {
        cz::Vector<cz::Str> variable_values = {};
        variable_values.reserve(allocator, parse.variable_values.len);
        for (size_t i = 0; i < parse.variable_values.len; ++i) {
            cz::String value = {};
            expand_arg_single(local, parse.variable_values[i], allocator, &value);
            variable_values.push(value);
        }
        parse.variable_values = variable_values;
    }

    if (parse.is_sub) {
        program->type = Running_Program::SUB;
        program->v.sub = {};
        program->v.sub.stdio = stdio;
        program->v.sub.local = allocator.alloc<Shell_Local>();
        *program->v.sub.local = {};
        program->v.sub.local->parent = local;
        return start_execute_node(shell, tty, backlog, &program->v.sub, parse.v.sub);
    }

    // Expand arguments.
    cz::Vector<cz::Str> args = {};
    CZ_DEFER(args.drop(cz::heap_allocator()));
    for (size_t i = 0; i < parse.v.args.len; ++i) {
        expand_arg_split(local, parse.v.args[i], allocator, &args);
    }

    if (parse.v.args.len > 0 || args.len > 0) {
        // Lookup aliases based on the raw arguments, functions based on the expanded arguments.
        cz::Str alias_key = (parse.v.args.len > 0 ? parse.v.args[0] : "");
        cz::Str function_key = (args.len > 0 ? args[0] : "");
        Shell_Node* body;
        int result = get_alias_or_function(local, alias_key, function_key, &body);

        if (result != 0) {
            program->type = Running_Program::SUB;
            program->v.sub = {};
            program->v.sub.stdio = stdio;
            program->v.sub.local = allocator.alloc<Shell_Local>();
            *program->v.sub.local = {};
            program->v.sub.local->parent = local;
            program->v.sub.local->args = args.clone(allocator);

            // Track the alias stack to prevent infinite recursion on 'alias ls=ls; ls'.
            if (result == 1) {
                program->v.sub.local->blocked_alias = parse.v.args[0];
            }

            return start_execute_node(shell, tty, backlog, &program->v.sub, body);
        }
    }

    parse.v.args = args;
    recognize_builtins(program, parse, allocator);

    // If command is a builtin.
    if (program->type != Running_Program::PROCESS) {
        if (stdio.in_type == File_Type_Pipe && !stdio.in.set_non_blocking())
            return Error_IO;
        if (stdio.out_type == File_Type_Pipe && !stdio.out.set_non_blocking())
            return Error_IO;
        if (stdio.err_type == File_Type_Pipe && !stdio.err.set_non_blocking())
            return Error_IO;

        if (stdio.in_type == File_Type_Terminal) {
            program->v.builtin.in.polling = true;
#ifdef _WIN32
            program->v.builtin.in.file = tty.child_in;
#else
            program->v.builtin.in.file.handle = tty.child_bi;
#endif
        } else {
            program->v.builtin.in.polling = false;
            program->v.builtin.in.file = stdio.in;
            program->v.builtin.in_count = stdio.in_count;
        }
        if (stdio.out_type == File_Type_Terminal) {
            program->v.builtin.out.type = Process_Output::BACKLOG;
            program->v.builtin.out.v.backlog = backlog;
        } else {
            program->v.builtin.out.type = Process_Output::FILE;
            program->v.builtin.out.v.file = stdio.out;
            program->v.builtin.out_count = stdio.out_count;
        }
        if (stdio.err_type == File_Type_Terminal) {
            program->v.builtin.err.type = Process_Output::BACKLOG;
            program->v.builtin.err.v.backlog = backlog;
        } else {
            program->v.builtin.err.type = Process_Output::FILE;
            program->v.builtin.err.v.file = stdio.err;
            program->v.builtin.err_count = stdio.err_count;
        }

        program->v.builtin.args = args.clone(allocator);
        program->v.builtin.working_directory = get_wd(local).clone_null_terminate(allocator);
        return Error_Success;
    }

    cz::String full_path = {};
    if (!find_in_path(local, args[0], allocator, &full_path)) {
        return Error_InvalidProgram;
    }
    args[0] = full_path;

#ifdef _WIN32
    if (args[0].ends_with_case_insensitive(".ps1")) {
        args.reserve(cz::heap_allocator(), 1);
        args.insert(0, "powershell");
    }
#endif

    cz::Process_Options options;
#ifdef _WIN32
    if (stdio.in_type == File_Type_Terminal && stdio.out_type == File_Type_Terminal &&
        stdio.err_type == File_Type_Terminal) {
        // TODO: test pseudo console + stdio.
        options.pseudo_console = tty.pseudo_console;
    } else {
        if (stdio.in_type == File_Type_Terminal) {
            options.std_in = tty.child_in;
        } else {
            options.std_in = stdio.in;
        }
        if (stdio.out_type == File_Type_Terminal) {
            options.std_out = tty.child_out;
        } else {
            options.std_out = stdio.out;
        }
        if (stdio.err_type == File_Type_Terminal) {
            options.std_err = tty.child_out;  // yes, out!
        } else {
            options.std_err = stdio.err;
        }
    }
#else
    if (stdio.in_type == File_Type_Terminal) {
        options.std_in.handle = tty.child_bi;
    } else {
        options.std_in = stdio.in;
    }
    if (stdio.out_type == File_Type_Terminal) {
        options.std_out.handle = tty.child_bi;
    } else {
        options.std_out = stdio.out;
    }
    if (stdio.err_type == File_Type_Terminal) {
        options.std_err.handle = tty.child_bi;
    } else {
        options.std_err = stdio.err;
    }
#endif

    options.working_directory = get_wd(local).buffer;
    generate_environment(&options.environment, shell, local, parse.variable_names,
                         parse.variable_values);

    program->v.process = {};
    bool result = program->v.process.launch_program(args, options);

    close_rc_file(stdio.in_count, stdio.in);
    close_rc_file(stdio.out_count, stdio.out);
    close_rc_file(stdio.err_count, stdio.err);

    if (!result)
        return Error_IO;
    return Error_Success;
}

///////////////////////////////////////////////////////////////////////////////
// Environment control
///////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32
static void push_environment(cz::String* table, cz::Str key, cz::Str value) {
    table->reserve(temp_allocator, key.len + value.len + 3);
    table->append(key);
    table->push('=');
    table->append(value);
    table->push('\0');
}
#else
static void push_environment(cz::Vector<char*>* table, cz::Str key, cz::Str value) {
    cz::String entry = {};
    entry.reserve_exact(temp_allocator, key.len + value.len + 2);
    entry.append(key);
    entry.push('=');
    entry.append(value);
    entry.null_terminate();

    table->reserve(cz::heap_allocator(), 1);
    table->push(entry.buffer);
}
#endif

static void generate_environment(void* out_arg,
                                 const Shell_State* shell,
                                 const Shell_Local* local,
                                 cz::Slice<const cz::Str> variable_names,
                                 cz::Slice<const cz::Str> variable_values) {
#ifdef _WIN32
    cz::String table = {};
#else
    cz::Vector<char*> table = {};
    CZ_DEFER(table.drop(cz::heap_allocator()));
#endif

    for (size_t i = 0; i < variable_names.len; ++i) {
        cz::Str key = variable_names[i];
        for (size_t j = 0; j < i; ++j) {
            if (key == variable_names[j])
                goto skip1;
        }
        push_environment(&table, key, variable_values[i]);
    skip1:;
    }

    for (size_t i = 0; i < shell->exported_vars.len; ++i) {
        cz::Str key = shell->exported_vars[i];
        for (size_t j = 0; j < variable_names.len; ++j) {
            if (key == variable_names[j])
                goto skip2;
        }
        for (size_t j = 0; j < i; ++j) {
            if (key == shell->exported_vars[j])
                goto skip2;
        }
        cz::Str value;
        if (!get_var(local, key, &value))
            value = {};
        push_environment(&table, key, value);
    skip2:;
    }

#ifdef _WIN32
    char** out = (char**)out_arg;
    table.null_terminate();
    *out = table.buffer;
#else
    char*** out = (char***)out_arg;
    table.reserve(cz::heap_allocator(), 1);
    table.push(nullptr);
    *out = table.clone(temp_allocator).elems;
#endif
}

///////////////////////////////////////////////////////////////////////////////
// Recognize builtins
///////////////////////////////////////////////////////////////////////////////

static void recognize_builtins(Running_Program* program,
                               const Parse_Program& parse,
                               cz::Allocator allocator) {
    program->type = Running_Program::PROCESS;

    // Line that only assigns to variables runs as special builtin.
    if (parse.v.args.len == 0) {
        CZ_ASSERT(parse.variable_names.len > 0);
        program->type = Running_Program::VARIABLES;
        program->v.builtin.st.variables = {};
        program->v.builtin.st.variables.names = parse.variable_names;
        program->v.builtin.st.variables.values = parse.variable_values;
        return;
    }

    // Strictly necessary builtins.
    if (cfg.builtin_level >= 0) {
        if (parse.v.args[0] == "exit") {
            program->type = Running_Program::EXIT;
        } else if (parse.v.args[0] == "return") {
            program->type = Running_Program::RETURN;
        } else if (parse.v.args[0] == "cd") {
            program->type = Running_Program::CD;
        } else if (parse.v.args[0] == "alias") {
            program->type = Running_Program::ALIAS;
        } else if (parse.v.args[0] == "export") {
            program->type = Running_Program::EXPORT;
        } else if (parse.v.args[0] == "clear") {
            program->type = Running_Program::CLEAR;
        } else if (parse.v.args[0] == "." || parse.v.args[0] == "source") {
            program->type = Running_Program::SOURCE;
        } else if (parse.v.args[0] == "sleep") {
            program->type = Running_Program::SLEEP;
            program->v.builtin.st.sleep = {};
            program->v.builtin.st.sleep.start = std::chrono::high_resolution_clock::now();
        } else if (parse.v.args[0] == "configure") {
            program->type = Running_Program::CONFIGURE;
        } else if (parse.v.args[0] == "attach") {
            program->type = Running_Program::ATTACH;
        } else if (parse.v.args[0] == "argdump") {
            program->type = Running_Program::ARGDUMP;
        } else if (parse.v.args[0] == "vardump") {
            program->type = Running_Program::VARDUMP;
        }
    }

    // Compromise builtins.
    if (cfg.builtin_level >= 1) {
        if (parse.v.args[0] == "echo") {
            program->type = Running_Program::ECHO;
            program->v.builtin.st.echo = {};
            program->v.builtin.st.echo.outer = 1;
        } else if (parse.v.args[0] == "pwd") {
            program->type = Running_Program::PWD;
        } else if (parse.v.args[0] == "which") {
            program->type = Running_Program::WHICH;
        } else if (parse.v.args[0] == "true") {
            program->type = Running_Program::TRUE_;
        } else if (parse.v.args[0] == "false") {
            program->type = Running_Program::FALSE_;
        }
    }

    // All builtins.
    if (cfg.builtin_level >= 2) {
        if (parse.v.args[0] == "cat") {
            program->type = Running_Program::CAT;
            program->v.builtin.st.cat = {};
            program->v.builtin.st.cat.buffer = (char*)allocator.alloc({4096, 1});
            program->v.builtin.st.cat.outer = 0;
        } else if (parse.v.args[0] == "ls") {
            program->type = Running_Program::LS;
        }
    }
}
