#include "shell.hpp"

#include <Tracy.hpp>
#include <cz/defer.hpp>
#include <cz/heap.hpp>
#include <cz/process.hpp>

#include "config.hpp"
#include "global.hpp"

///////////////////////////////////////////////////////////////////////////////

static Error start_execute_pipeline(Shell_State* shell,
                                    Backlog_State* backlog,
                                    cz::Buffer_Array arena,
                                    const Running_Script& script,
                                    const Parse_Pipeline& parse,
                                    Running_Pipeline* running);

static Error run_program(Shell_State* shell,
                         cz::Allocator allocator,
                         Running_Program* program,
                         Parse_Program parse,
                         cz::Input_File in,
                         Process_Output out,
                         Process_Output err,
                         bool in_pipe,
                         bool out_pipe,
                         bool err_pipe);

static void recognize_builtins(Running_Program* program,
                               const Parse_Program& parse,
                               cz::Allocator allocator);

///////////////////////////////////////////////////////////////////////////////

Error start_execute_script(Shell_State* shell,
                           Backlog_State* backlog,
                           cz::Buffer_Array arena,
                           const Parse_Script& parse,
                           cz::Str command_line,
                           uint64_t id) {
    Error error = Error_Success;

    Running_Script running = {};
    running.id = id;

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

    running.arena = arena;

    error = start_execute_line(shell, backlog, &running, parse.first);
    if (error != Error_Success)
        goto cleanup2;

    shell->scripts.reserve(cz::heap_allocator(), 1);
    shell->scripts.push(running);

    if (cfg.on_spawn_attach)
        shell->active_process = running.id;

    return Error_Success;

cleanup2:
    running.script_out.close();
    running.out.close();
cleanup1:
    running.script_in.close();
    running.in.close();
    return error;
}

///////////////////////////////////////////////////////////////////////////////

Error start_execute_line(Shell_State* shell,
                         Backlog_State* backlog,
                         Running_Script* running,
                         const Parse_Line& line) {
    running->fg.on = line.on;

    // TODO: I think we should refactor this to create the
    // Running_Script then adding a Running_Line to it.  Idk.
    cz::Buffer_Array pipeline_arena = alloc_arena(shell);
    Error error = start_execute_pipeline(shell, backlog, pipeline_arena, *running, line.pipeline,
                                         &running->fg.pipeline);
    if (error != Error_Success)
        recycle_arena(shell, pipeline_arena);
    return error;
}

///////////////////////////////////////////////////////////////////////////////

static Error start_execute_pipeline(Shell_State* shell,
                                    Backlog_State* backlog,
                                    cz::Buffer_Array arena,
                                    const Running_Script& script,
                                    const Parse_Pipeline& parse,
                                    Running_Pipeline* running) {
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

    cz::Input_File pipe_in = script.script_in;

    for (size_t p = 0; p < parse.pipeline.len; ++p) {
        Parse_Program parse_program = parse.pipeline[p];

        // TODO: This is the wrong place to expand aliases.
        for (size_t i = 0; i < shell->alias_names.len; ++i) {
            if (parse_program.args[0] == shell->alias_names[i]) {
                cz::Vector<cz::Str> args2 = parse_program.args.clone(arena.allocator());
                args2[0] = shell->alias_values[i];
                parse_program.args = args2;
                break;
            }
        }

        Running_Program running_program = {};
        recognize_builtins(&running_program, parse_program, arena.allocator());

        Process_Output my_line_out = {};
        if (running_program.type == Running_Program::PROCESS) {
            my_line_out.type = Process_Output::FILE;
            my_line_out.v.file = script.script_out;
        } else {
            my_line_out.type = Process_Output::BACKLOG;
            my_line_out.v.backlog.state = backlog;
            my_line_out.v.backlog.process_id = script.id;
        }

        bool in_pipe = false, out_pipe = false, err_pipe = false;
        cz::Input_File in;
        Process_Output out = {};
        Process_Output err = {};

        if (parse_program.in_file.buffer) {
            if (!in.open(parse_program.in_file.buffer))
                return Error_InvalidPath;  // TODO: cleanup file descriptors
            files.reserve(cz::heap_allocator(), 1);
            files.push(in);
            pipe_in = {};
        } else {
            in = pipe_in;
            pipe_in = {};
            in_pipe = true;
        }

        if (parse_program.out_file.buffer) {
            out.type = Process_Output::FILE;
            if (!out.v.file.open(parse_program.out_file.buffer))
                return Error_InvalidPath;  // TODO: cleanup file descriptors
            files.reserve(cz::heap_allocator(), 1);
            files.push(out.v.file);
        } else {
            if (p + 1 == parse.pipeline.len) {
                out = my_line_out;
            } else {
                cz::Output_File pipe_out;
                if (!cz::create_pipe(&pipe_in, &pipe_out))
                    return Error_IO;
                files.reserve(cz::heap_allocator(), 2);
                files.push(pipe_in);
                files.push(pipe_out);
                out.type = Process_Output::FILE;
                out.v.file = pipe_out;
            }
            out_pipe = true;
        }

        if (parse_program.err_file.buffer) {
            err.type = Process_Output::FILE;
            if (!err.v.file.open(parse_program.err_file.buffer))
                return Error_InvalidPath;  // TODO: cleanup file descriptors
            files.reserve(cz::heap_allocator(), 1);
            files.push(err.v.file);
        } else {
            err = my_line_out;
            err_pipe = true;
        }

        Error error = run_program(shell, arena.allocator(), &running_program, parse_program, in,
                                  out, err, in_pipe, out_pipe, err_pipe);
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
                         cz::Input_File in,
                         Process_Output out,
                         Process_Output err,
                         bool in_pipe,
                         bool out_pipe,
                         bool err_pipe) {
    // If command is a builtin.
    if (program->type != Running_Program::PROCESS) {
        if (in_pipe && !in.set_non_blocking())
            return Error_IO;
        if (out_pipe && out.type == Process_Output::FILE && !out.v.file.set_non_blocking())
            return Error_IO;
        if (err_pipe && err.type == Process_Output::FILE && !err.v.file.set_non_blocking())
            return Error_IO;

        program->v.builtin.args = parse.args;
        program->v.builtin.in = in;
        program->v.builtin.out = out;
        program->v.builtin.err = err;
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

    parse.args = args;

    CZ_DEBUG_ASSERT(out.type == Process_Output::FILE);
    CZ_DEBUG_ASSERT(err.type == Process_Output::FILE);

    cz::Process_Options options;
    options.std_in = in;
    options.std_out = out.v.file;
    options.std_err = err.v.file;
    options.working_directory = shell->working_directory.buffer;
    generate_environment(&options.environment, shell, parse.variable_names, parse.variable_values);

    program->v.process = {};
    if (!program->v.process.launch_program(parse.args, options))
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
    }
}
