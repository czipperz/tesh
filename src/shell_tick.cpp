#include "shell.hpp"

#include <stdlib.h>
#include <Tracy.hpp>
#include <cz/format.hpp>
#include <cz/heap.hpp>
#include <cz/parse.hpp>

bool tick_program(Running_Program* program, int* exit_code) {
    ZoneScoped;

    switch (program->type) {
    case Running_Program::PROCESS:
        return program->v.process.try_join(exit_code);

    case Running_Program::ECHO: {
        auto& builtin = program->v.builtin;
        auto& st = builtin.st.echo;
        int64_t result = 0;
        int rounds = 0;
        for (; st.outer < builtin.args.len; ++st.outer) {
            // Rate limit to prevent hanging.
            if (rounds++ == 16)
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
            result = builtin.out.write("\n", 1);

        if (result > 0) {
            CZ_DEBUG_ASSERT(st.outer == builtin.args.len);
            goto finish_builtin;
        }
    } break;

    case Running_Program::CAT: {
        auto& builtin = program->v.builtin;
        auto& st = builtin.st.cat;

        if (builtin.args.len == 1 && builtin.in.is_open()) {
            st.file = builtin.in;
            builtin.in = {};
        }

        int64_t result = 0;
        int rounds = 0;
        while (st.file.is_open() || st.outer < builtin.args.len) {
            // Rate limit to prevent hanging.
            if (rounds++ == 16)
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

            // Read a new buffer.
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

    case Running_Program::EXIT: {
        auto& builtin = program->v.builtin;
        if (builtin.args.len == 1)
            exit(0);
        int num = 1;
        cz::parse(builtin.args[1], &num);
        exit(num);
    } break;

    default:
        CZ_PANIC("unreachable");
    }
    return false;

finish_builtin:
    auto& builtin = program->v.builtin;
    builtin.in.close();
    builtin.out.close();
    if (builtin.close_err)
        builtin.err.close();
    return true;
}
