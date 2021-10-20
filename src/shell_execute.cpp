#include "shell.hpp"

#include <Tracy.hpp>
#include <cz/defer.hpp>
#include <cz/heap.hpp>
#include <cz/process.hpp>

///////////////////////////////////////////////////////////////////////////////

static Error run_program(Running_Program* program,
                         cz::Slice<const cz::Str> args,
                         cz::Input_File in,
                         cz::Output_File out,
                         cz::Output_File err);

///////////////////////////////////////////////////////////////////////////////

Error start_execute_line(Shell_State* shell, const Parse_Line& parse_line, uint64_t id) {
    ZoneScoped;

    if (parse_line.pipeline.len == 0)
        return Error_Success;

    Running_Line running_line = {};
    running_line.id = id;

    cz::Input_File line_in;
    cz::Output_File line_out;
    if (!cz::create_process_input_pipe(&line_in, &running_line.in))
        return Error_IO;
    if (!cz::create_process_output_pipe(&line_out, &running_line.out))
        return Error_IO;
    if (!running_line.in.set_non_blocking())
        return Error_IO;
    if (!running_line.out.set_non_blocking())
        return Error_IO;

    cz::Input_File pipe_in;
    cz::Output_File pipe_out;

    for (size_t p = 0; p < parse_line.pipeline.len; ++p) {
        const Parse_Program& parse_program = parse_line.pipeline[p];
        cz::Input_File in;
        cz::Output_File out;
        cz::Output_File err = line_out;

        if (p == 0) {
            in = line_in;
            line_in = {};
        } else
            in = pipe_in;

        if (p + 1 == parse_line.pipeline.len) {
            out = line_out;
            line_out = {};
        } else {
            if (!cz::create_pipe(&pipe_in, &pipe_out))
                return Error_IO;
            out = pipe_out;
        }

        Running_Program running_program = {};
        Error error = run_program(&running_program, parse_program.args, in, out, err);
        if (error != Error_Success)
            return error;

        running_line.pipeline.reserve(cz::heap_allocator(), 1);
        running_line.pipeline.push(running_program);
    }

    shell->lines.reserve(cz::heap_allocator(), 1);
    shell->lines.push(running_line);

    return Error_Success;
}

///////////////////////////////////////////////////////////////////////////////

static Error run_program(Running_Program* program,
                         cz::Slice<const cz::Str> args,
                         cz::Input_File in,
                         cz::Output_File out,
                         cz::Output_File err) {
    program->type = Running_Program::PROCESS;

    // Setup builtins.
    if (args[0] == "echo") {
        program->type = Running_Program::ECHO;
        program->v.builtin.st.echo = {};
        program->v.builtin.st.echo.outer = 1;
    }
    if (args[0] == "cat") {
        program->type = Running_Program::CAT;
        program->v.builtin.st.cat = {};
        program->v.builtin.st.cat.buffer = (char*)cz::heap_allocator().alloc({4096, 1});
        program->v.builtin.st.cat.outer = 1;
    }

    // If command is a builtin.
    if (program->type != Running_Program::PROCESS) {
        if (!in.set_non_blocking())
            return Error_IO;
        if (!out.set_non_blocking())
            return Error_IO;
        if (!err.set_non_blocking())
            return Error_IO;

        program->v.builtin.args = args;
        program->v.builtin.in = in;
        program->v.builtin.out = out;
        program->v.builtin.err = err;
        return Error_Success;
    }

    cz::Process_Options options;
    options.std_in = in;
    options.std_out = out;
    options.std_err = err;
    CZ_DEFER(options.close_all());

    program->v.process = {};
    if (!program->v.process.launch_program(args, options))
        return Error_IO;

    return Error_Success;
}
