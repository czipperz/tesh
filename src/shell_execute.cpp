#include "shell.hpp"

#include <Tracy.hpp>
#include <cz/defer.hpp>
#include <cz/heap.hpp>
#include <cz/process.hpp>

#include "config.hpp"
#include "global.hpp"

///////////////////////////////////////////////////////////////////////////////

enum File_Type {
    File_Type_Terminal,
    File_Type_File,
    File_Type_Pipe,
    File_Type_None,
};

struct Stdio_State {
    File_Type in_type = File_Type_Terminal;
    File_Type out_type = File_Type_Terminal;
    File_Type err_type = File_Type_Terminal;
    cz::Input_File in;
    cz::Output_File out;
    cz::Output_File err;
};

///////////////////////////////////////////////////////////////////////////////

static Error start_execute_pipeline(Shell_State* shell,
                                    Backlog_State* backlog,
                                    cz::Buffer_Array arena,
                                    const Running_Script& script,
                                    const Parse_Pipeline& parse,
                                    Running_Pipeline* running,
                                    bool bind_stdin);

static Error run_program(Shell_State* shell,
                         cz::Allocator allocator,
                         Running_Program* program,
                         Parse_Program parse,
                         Stdio_State stdio,
                         Backlog_State* backlog,
                         const Running_Script& script);

static void recognize_builtins(Running_Program* program,
                               const Parse_Program& parse,
                               cz::Allocator allocator);

///////////////////////////////////////////////////////////////////////////////

Error start_execute_script(Shell_State* shell,
                           Backlog_State* backlog,
                           cz::Buffer_Array arena,
                           const Parse_Script& parse,
                           cz::Str command_line) {
    Error error = Error_Success;

    Running_Script running = {};
    running.id = backlog->id;
    running.arena = arena;

    if (!create_pseudo_terminal(&running.tty, shell->width, shell->height))
        return Error_IO;

#if 0
    if (!cz::create_process_input_pipe(&running.script_in, &running.in))
        return Error_IO;
    if (!cz::create_process_output_pipe(&running.script_out, &running.out)) {
        error = Error_IO;
        goto cleanup1;
    }

    if (!running.in.set_non_blocking()) {
        error = Error_IO;
        goto cleanup2;
    }
    if (!running.out.set_non_blocking()) {
        error = Error_IO;
        goto cleanup2;
    }
#endif

    error = start_execute_line(shell, backlog, &running, parse.first, /*background=*/false);
    if (error != Error_Success) {
        destroy_pseudo_terminal(&running.tty);
        return error;
    }

    shell->scripts.reserve(cz::heap_allocator(), 1);
    shell->scripts.push(running);

    if (cfg.on_spawn_attach)
        shell->active_process = running.id;

    return Error_Success;

#if 0
cleanup2:
    running.script_out.close();
    running.out.close();
cleanup1:
    running.script_in.close();
    running.in.close();
    return error;
#endif
}

///////////////////////////////////////////////////////////////////////////////

Error start_execute_line(Shell_State* shell,
                         Backlog_State* backlog,
                         Running_Script* running,
                         const Parse_Line& parse_in,
                         bool background) {
    const Parse_Line* parse = &parse_in;

again:
    Running_Line* line = &running->fg;
    bool bind_stdin = true;

    // If another line starts at the start of this line, then
    // this line is async and should be ran in the background.
    if (parse->on.start || background) {
        running->bg.reserve(cz::heap_allocator(), 1);
        running->bg.push({});
        line = &running->bg.last();
        bind_stdin = false;
    }

    line->on = parse->on;

    // TODO: I think we should refactor this to create the
    // Running_Script then adding a Running_Line to it.  Idk.
    cz::Buffer_Array pipeline_arena = alloc_arena(shell);
    Error error = start_execute_pipeline(shell, backlog, pipeline_arena, *running, parse->pipeline,
                                         &line->pipeline, bind_stdin);
    if (error != Error_Success) {
        recycle_arena(shell, pipeline_arena);
        return error;
    }

    // Recurse on the next line since this one is async.
    if (parse->on.start) {
        parse = parse->on.start;
        goto again;
    }

    return Error_Success;
}

///////////////////////////////////////////////////////////////////////////////

static Error start_execute_pipeline(Shell_State* shell,
                                    Backlog_State* backlog,
                                    cz::Buffer_Array arena,
                                    const Running_Script& script,
                                    const Parse_Pipeline& parse,
                                    Running_Pipeline* running,
                                    bool bind_stdin) {
    ZoneScoped;

    running->length = parse.pipeline.len;

    if (parse.pipeline.len == 0)
        return Error_Empty;

    running->arena = arena;
    // running->command_line = command_line.clone(arena.allocator());

    cz::Vector<Running_Program> pipeline = {};
    CZ_DEFER(pipeline.drop(cz::heap_allocator()));
    cz::Vector<cz::File_Descriptor> files = {};
    CZ_DEFER({
        for (size_t i = 0; i < files.len; ++i) {
            files[i].close();
        }
        files.drop(cz::heap_allocator());
    });

    cz::Input_File pipe_in;

    for (size_t p = 0; p < parse.pipeline.len; ++p) {
        Parse_Program parse_program = parse.pipeline[p];

        Running_Program running_program = {};
        recognize_builtins(&running_program, parse_program, arena.allocator());

        Stdio_State stdio = {};

        if (parse_program.in_file.buffer) {
            stdio.in_type = File_Type_File;
            if (!stdio.in.open(parse_program.in_file.buffer))
                return Error_InvalidPath;
            files.reserve(cz::heap_allocator(), 1);
            files.push(stdio.in);
        } else if (p > 0) {
            stdio.in_type = File_Type_Pipe;
            stdio.in = pipe_in;
        } else if (!bind_stdin) {
            stdio.in_type = File_Type_None;
        }
        pipe_in = {};

        if (parse_program.out_file.buffer) {
            stdio.out_type = File_Type_File;
            if (!stdio.out.open(parse_program.out_file.buffer))
                return Error_InvalidPath;
            files.reserve(cz::heap_allocator(), 1);
            files.push(stdio.out);
        } else if (p + 1 < parse.pipeline.len) {
            stdio.out_type = File_Type_Pipe;
        }

        if (parse_program.err_file.buffer) {
            stdio.err_type = File_Type_File;
            if (!stdio.err.open(parse_program.err_file.buffer))
                return Error_InvalidPath;
            files.reserve(cz::heap_allocator(), 1);
            files.push(stdio.err);
        }

        // Make pipes for the next iteration.
        if (stdio.out_type == File_Type_Pipe || stdio.err_type == File_Type_Pipe) {
            cz::Output_File pipe_out;
            if (!cz::create_pipe(&pipe_in, &pipe_out))
                return Error_IO;
            files.reserve(cz::heap_allocator(), 2);
            files.push(pipe_in);
            files.push(pipe_out);

            if (stdio.out_type == File_Type_Pipe)
                stdio.out = pipe_out;
            if (stdio.err_type == File_Type_Pipe)
                stdio.err = pipe_out;
        }

        Error error = run_program(shell, arena.allocator(), &running_program, parse_program, stdio,
                                  backlog, script);
        if (error != Error_Success)
            return error;

        pipeline.reserve(cz::heap_allocator(), 1);
        pipeline.push(running_program);
    }

    running->pipeline = pipeline.clone(arena.allocator());
    running->files = files.clone(arena.allocator());
    pipeline.len = 0;
    files.len = 0;

    return Error_Success;
}

///////////////////////////////////////////////////////////////////////////////

static void generate_environment(void* out,
                                 Shell_State* shell,
                                 cz::Slice<const cz::Str> variable_names,
                                 cz::Slice<const cz::Str> variable_values);

static Error run_program(Shell_State* shell,
                         cz::Allocator allocator,
                         Running_Program* program,
                         Parse_Program parse,
                         Stdio_State stdio,
                         Backlog_State* backlog,
                         const Running_Script& script) {
    // If command is a builtin.
    if (program->type != Running_Program::PROCESS) {
        if (stdio.in_type == File_Type_Pipe && !stdio.in.set_non_blocking())
            return Error_IO;
        if (stdio.out_type == File_Type_Pipe && !stdio.out.set_non_blocking())
            return Error_IO;
        if (stdio.err_type == File_Type_Pipe && !stdio.err.set_non_blocking())
            return Error_IO;

        if (stdio.in_type == File_Type_Terminal) {
#ifdef _WIN32
            program->v.builtin.in = script.tty.child_in;
#else
            program->v.builtin.in.handle = script.tty.child_bi;
#endif
        } else {
            program->v.builtin.in = stdio.in;
        }
        if (stdio.out_type == File_Type_Terminal) {
            program->v.builtin.out.type = Process_Output::BACKLOG;
            program->v.builtin.out.v.backlog = backlog;
        } else {
            program->v.builtin.out.type = Process_Output::FILE;
            program->v.builtin.out.v.file = stdio.out;
        }
        if (stdio.err_type == File_Type_Terminal) {
            program->v.builtin.err.type = Process_Output::BACKLOG;
            program->v.builtin.err.v.backlog = backlog;
        } else {
            program->v.builtin.err.type = Process_Output::FILE;
            program->v.builtin.err.v.file = stdio.err;
        }

        program->v.builtin.args = parse.args;
        program->v.builtin.working_directory =
            shell->working_directory.clone_null_terminate(allocator);
        return Error_Success;
    }

    cz::Vector<cz::Str> args = parse.args.clone(temp_allocator);

    cz::String full_path = {};
    if (!find_in_path(shell, args[0], allocator, &full_path)) {
        return Error_InvalidProgram;
    }
    args[0] = full_path;

#ifdef _WIN32
    if (args[0].ends_with_case_insensitive(".ps1")) {
        args.reserve(temp_allocator, 1);
        args.insert(0, "powershell");
    }
#endif

    cz::Process_Options options;
#ifdef _WIN32
    if (stdio.in_type == File_Type_Terminal && stdio.out_type == File_Type_Terminal &&
        stdio.err_type == File_Type_Terminal) {
        // TODO: test pseudo console + stdio.
        options.pseudo_console = script.tty.pseudo_console;
    } else {
        if (stdio.in_type == File_Type_Terminal) {
            options.std_in = script.tty.child_in;
        } else {
            options.std_in = stdio.in;
        }
        if (stdio.out_type == File_Type_Terminal) {
            options.std_out = script.tty.child_out;
        } else {
            options.std_out = stdio.out;
        }
        if (stdio.err_type == File_Type_Terminal) {
            options.std_err = script.tty.child_out;  // yes, out!
        } else {
            options.std_err = stdio.err;
        }
    }
#else
    if (stdio.in_type == File_Type_Terminal) {
        options.std_in.handle = script.tty.child_bi;
    } else {
        options.std_in = stdio.in;
    }
    if (stdio.out_type == File_Type_Terminal) {
        options.std_out.handle = script.tty.child_bi;
    } else {
        options.std_out = stdio.out;
    }
    if (stdio.err_type == File_Type_Terminal) {
        options.std_err.handle = script.tty.child_bi;
    } else {
        options.std_err = stdio.err;
    }
#endif

    options.working_directory = shell->working_directory.buffer;
    generate_environment(&options.environment, shell, parse.variable_names, parse.variable_values);

    program->v.process = {};
    if (!program->v.process.launch_program(args, options))
        return Error_IO;

    return Error_Success;
}

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
                                 Shell_State* shell,
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
        if (!get_var(shell, key, &value))
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

static void recognize_builtins(Running_Program* program,
                               const Parse_Program& parse,
                               cz::Allocator allocator) {
    program->type = Running_Program::PROCESS;

    if (parse.args.len == 0) {
        CZ_ASSERT(parse.variable_names.len > 0);
        program->type = Running_Program::VARIABLES;
        program->v.builtin.st.variables = {};
        program->v.builtin.st.variables.names = parse.variable_names;
        program->v.builtin.st.variables.values = parse.variable_values;
        return;
    }

    // Setup builtins.
    if (parse.args[0] == "echo") {
        program->type = Running_Program::ECHO;
        program->v.builtin.st.echo = {};
        program->v.builtin.st.echo.outer = 1;
    } else if (parse.args[0] == "cat") {
        program->type = Running_Program::CAT;
        program->v.builtin.st.cat = {};
        program->v.builtin.st.cat.buffer = (char*)allocator.alloc({4096, 1});
        program->v.builtin.st.cat.outer = 0;
    } else if (parse.args[0] == "exit") {
        program->type = Running_Program::EXIT;
    } else if (parse.args[0] == "return") {
        program->type = Running_Program::RETURN;
    } else if (parse.args[0] == "pwd") {
        program->type = Running_Program::PWD;
    } else if (parse.args[0] == "cd") {
        program->type = Running_Program::CD;
    } else if (parse.args[0] == "ls") {
        program->type = Running_Program::LS;
    } else if (parse.args[0] == "alias") {
        program->type = Running_Program::ALIAS;
    } else if (parse.args[0] == "which") {
        program->type = Running_Program::WHICH;
    } else if (parse.args[0] == "true") {
        program->type = Running_Program::TRUE_;
    } else if (parse.args[0] == "false") {
        program->type = Running_Program::FALSE_;
    } else if (parse.args[0] == "export") {
        program->type = Running_Program::EXPORT;
    } else if (parse.args[0] == "clear") {
        program->type = Running_Program::CLEAR;
    } else if (parse.args[0] == "." || parse.args[0] == "source") {
        program->type = Running_Program::SOURCE;
    } else if (parse.args[0] == "sleep") {
        program->type = Running_Program::SLEEP;
        program->v.builtin.st.sleep = {};
        program->v.builtin.st.sleep.start = std::chrono::high_resolution_clock::now();
    }
}
