#include "shell.hpp"

#include <cz/format.hpp>
#include <cz/heap.hpp>

#include <windows.h>

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
            goto finish_builtin;
        }
    } break;

    case Running_Program::CAT: {
        auto& builtin = program->v.builtin;
        auto& st = builtin.st.cat;
        int64_t result = 0;
        int rounds = 0;
        while (st.outer < builtin.args.len) {
            // Rate limit for performance reasons.
            if (rounds++ == 16)
                return false;

            if (st.offset != st.len) {
                result = builtin.out.write(st.buffer + st.offset, st.len - st.offset);
                if (result <= 0)
                    return false;

                st.offset += result;
                if (st.offset != st.len)
                    return false;
            }

            if (!st.file.is_open()) {
                cz::Str arg = builtin.args[st.outer];
                if (arg == "-") {
                    st.file = builtin.in;
                    builtin.in = {};
                } else {
                    if (!st.file.open(arg.clone_null_terminate(cz::heap_allocator()).buffer)) {
                        cz::Str message = cz::format("cat: ", arg, ": No such file or directory\n");
                        (void)builtin.err.write(message.buffer, message.len);
                        continue;
                    }
                }
            }

            result = st.file.read(st.buffer, 4096);
            if (result <= 0) {
                if (result < 0)
                    break;
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

    default:
        CZ_PANIC("unreachable");
    }
    return false;

finish_builtin:
    auto& builtin = program->v.builtin;
    builtin.in.close();
    builtin.out.close();
    if (builtin.out.handle != builtin.err.handle)
        builtin.err.close();
    return true;
}
