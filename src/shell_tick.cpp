#include "shell.hpp"

bool tick_program(Running_Program* program, int* exit_code) {
    switch (program->type) {
    case Running_Program::PROCESS:
        return program->v.process.try_join(exit_code);

    case Running_Program::ECHO: {
        auto& builtin = program->v.builtin;
        auto& st = builtin.st.echo;
        int64_t result = 0;
        for (; st.outer < builtin.args.len; ++st.outer) {
            cz::Str arg = builtin.args[st.outer];
            if (st.inner != arg.len) {
                result = builtin.out.write(arg.buffer + st.inner, arg.len - st.inner);
                if (result <= 0)
                    break;

                st.inner += result;
                if (st.inner != arg.len)
                    break;
            }

            if (st.outer + 1 < builtin.args.len) {
                result = builtin.out.write(" ", 1);
                if (result <= 0)
                    break;
                st.inner = 0;
            }
        }

        if (st.outer == builtin.args.len)
            result = builtin.out.write("\n", 1);

        if (result > 0) {
            CZ_DEBUG_ASSERT(st.outer == builtin.args.len);
            builtin.in.close();
            builtin.out.close();
            if (builtin.out.handle != builtin.err.handle)
                builtin.err.close();
            return true;
        }
    } break;

    default:
        CZ_PANIC("unreachable");
    }
    return false;
}
