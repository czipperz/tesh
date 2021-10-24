#include "shell.hpp"

#include <Tracy.hpp>
#include <cz/defer.hpp>
#include <cz/heap.hpp>
#include <cz/process.hpp>

///////////////////////////////////////////////////////////////////////////////

static Error run_program(Shell_State* shell,
                         cz::Allocator allocator,
                         Running_Program* program,
                         Parse_Program parse,
                         cz::Input_File in,
                         cz::Output_File out,
                         cz::Output_File err,
                         bool in_pipe,
                         bool out_pipe,
                         bool err_pipe);

///////////////////////////////////////////////////////////////////////////////

Error start_execute_line(Shell_State* shell,
                         cz::Buffer_Array arena,
                         const Parse_Line& parse_line,
                         uint64_t id) {
    ZoneScoped;

    if (parse_line.pipeline.len == 0)
        return Error_Success;

    Running_Line running_line = {};
    running_line.id = id;
    running_line.arena = arena;
    CZ_DEFER(running_line.pipeline.drop(cz::heap_allocator()));

    cz::Input_File line_in;
    cz::Output_File line_out;
    if (!cz::create_process_input_pipe(&line_in, &running_line.in)) {
    cleanup1:
        line_in.close();
        line_out.close();
        running_line.in.close();
        running_line.out.close();
        return Error_IO;
    }
    if (!cz::create_process_output_pipe(&line_out, &running_line.out))
        goto cleanup1;
    if (!running_line.in.set_non_blocking())
        goto cleanup1;
    if (!running_line.out.set_non_blocking())
        goto cleanup1;

    cz::Input_File pipe_in = line_in;

    for (size_t p = 0; p < parse_line.pipeline.len; ++p) {
        const Parse_Program& parse_program = parse_line.pipeline[p];
        bool in_pipe = false, out_pipe = false, err_pipe = false;
        cz::Input_File in;
        cz::Output_File out;
        cz::Output_File err;

        if (parse_program.in_file.buffer) {
            if (!in.open(parse_program.in_file.buffer))
                return Error_InvalidPath;  // TODO: cleanup file descriptors
            pipe_in.close();
        } else {
            in = pipe_in;
            pipe_in = {};
            in_pipe = true;
        }

        if (parse_program.out_file.buffer) {
            if (!out.open(parse_program.out_file.buffer))
                return Error_InvalidPath;  // TODO: cleanup file descriptors
        } else {
            if (p + 1 == parse_line.pipeline.len) {
                out = line_out;
            } else {
                cz::Output_File pipe_out;
                if (!cz::create_pipe(&pipe_in, &pipe_out))
                    return Error_IO;
                out = pipe_out;
            }
            out_pipe = true;
        }

        if (parse_program.err_file.buffer) {
            if (!err.open(parse_program.err_file.buffer))
                return Error_InvalidPath;  // TODO: cleanup file descriptors
        } else {
            err = line_out;
            err_pipe = true;
        }

        Running_Program running_program = {};
        Error error = run_program(shell, arena.allocator(), &running_program, parse_program, in,
                                  out, err, in_pipe, out_pipe, err_pipe);
        if (error != Error_Success)
            return error;

        running_line.pipeline.reserve(cz::heap_allocator(), 1);
        running_line.pipeline.push(running_program);
    }

#ifdef ATTACH_ON_SPAWN
    shell->active_process = running_line.id;
#endif

    shell->lines.reserve(cz::heap_allocator(), 1);
    shell->lines.push(running_line);
    shell->lines.last().pipeline = running_line.pipeline.clone(arena.allocator());

    return Error_Success;
}

///////////////////////////////////////////////////////////////////////////////

static Error run_program(Shell_State* shell,
                         cz::Allocator allocator,
                         Running_Program* program,
                         Parse_Program parse,
                         cz::Input_File in,
                         cz::Output_File out,
                         cz::Output_File err,
                         bool in_pipe,
                         bool out_pipe,
                         bool err_pipe) {
    program->type = Running_Program::PROCESS;

    if (parse.args.len == 0) {
        CZ_ASSERT(parse.variable_names.len > 0);
        program->type = Running_Program::VARIABLES;
        program->v.builtin.st.variables = {};
        program->v.builtin.st.variables.names = parse.variable_names;
        program->v.builtin.st.variables.values = parse.variable_values;
        goto builtin;
    }

    // TODO: This is the wrong place to expand aliases.
    for (size_t i = 0; i < shell->alias_names.len; ++i) {
        if (parse.args[0] == shell->alias_names[i]) {
            cz::Vector<cz::Str> args2 = parse.args.clone(allocator);
            args2[0] = shell->alias_values[i];
            parse.args = args2;
            break;
        }
    }

    // Setup builtins.
    if (parse.args[0] == "echo") {
        program->type = Running_Program::ECHO;
        program->v.builtin.st.echo = {};
        program->v.builtin.st.echo.outer = 1;
    }
    if (parse.args[0] == "cat") {
        program->type = Running_Program::CAT;
        program->v.builtin.st.cat = {};
        program->v.builtin.st.cat.buffer = (char*)allocator.alloc({4096, 1});
        program->v.builtin.st.cat.outer = 1;
    }
    if (parse.args[0] == "exit") {
        program->type = Running_Program::EXIT;
    }
    if (parse.args[0] == "return") {
        program->type = Running_Program::RETURN;
    }
    if (parse.args[0] == "pwd") {
        program->type = Running_Program::PWD;
    }
    if (parse.args[0] == "cd") {
        program->type = Running_Program::CD;
    }
    if (parse.args[0] == "ls") {
        program->type = Running_Program::LS;
    }
    if (parse.args[0] == "alias") {
        program->type = Running_Program::ALIAS;
    }

    // If command is a builtin.
    if (program->type != Running_Program::PROCESS) {
    builtin:
        if (in_pipe && !in.set_non_blocking())
            return Error_IO;
        if (out_pipe && !out.set_non_blocking())
            return Error_IO;
        if (err_pipe && !err.set_non_blocking())
            return Error_IO;

        program->v.builtin.args = parse.args;
        program->v.builtin.in = in;
        program->v.builtin.out = out;
        program->v.builtin.err = err;
        program->v.builtin.working_directory =
            shell->working_directory.clone_null_terminate(allocator);
        return Error_Success;
    }

    cz::Process_Options options;
    options.std_in = in;
    options.std_out = out;
    options.std_err = err;
    options.working_directory = shell->working_directory.buffer;
    CZ_DEFER(options.close_all());

    program->v.process = {};
    if (!program->v.process.launch_program(parse.args, options))
        return Error_IO;

    return Error_Success;
}
