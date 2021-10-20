#include "shell_execute.hpp"

#include <cz/heap.hpp>
#include <cz/process.hpp>

///////////////////////////////////////////////////////////////////////////////

static Error run_program(Running_Program* program,
                         cz::Slice<const cz::Str> args,
                         cz::Input_File in,
                         cz::Output_File out,
                         cz::Output_File err);

///////////////////////////////////////////////////////////////////////////////

Error start_execute_line(Shell_State* shell, const Parse_Line& line, uint64_t id) {
    if (line.pipeline.len == 0)
        return ERROR_SUCCESS;

    cz::Output_File line_in_rev;
    cz::Input_File line_out_rev;

    cz::Input_File line_in;
    cz::Output_File line_out;
    if (!cz::create_pipe(&line_in, &line_in_rev))
        return ERROR_IO;
    if (!cz::create_pipe(&line_out_rev, &line_out))
        return ERROR_IO;

    cz::Input_File pipe_in;
    cz::Output_File pipe_out;

    for (size_t p = 0; p < line.pipeline.len; ++p) {
        const Parse_Program& parse_program = line.pipeline[p];
        cz::Input_File in;
        cz::Output_File out;
        cz::Output_File err = line_out;

        if (p == 0)
            in = line_in;
        else
            in = pipe_in;

        if (p + 1 == line.pipeline.len)
            out = line_out;
        else {
            if (!cz::create_pipe(&pipe_in, &pipe_out))
                return ERROR_IO;
            out = pipe_out;
        }

        Running_Program running_program = {};
        running_program.id = id;
        Error error = run_program(&running_program, parse_program.args, in, out, err);
        if (error != ERROR_SUCCESS)
            return error;

        shell->programs.reserve(cz::heap_allocator(), 1);
        shell->programs.push(running_program);
    }

    return ERROR_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////

static Error run_program(Running_Program* program,
                         cz::Slice<const cz::Str> args,
                         cz::Input_File in,
                         cz::Output_File out,
                         cz::Output_File err) {
    cz::Process_Options options;
    options.std_in = in;
    options.std_out = out;
    options.std_out = err;

    if (!program->process.launch_program(args, options))
        return ERROR_IO;

    return ERROR_SUCCESS;
}
