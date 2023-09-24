#include "shell.hpp"

#include <tracy/Tracy.hpp>
#include "prompt.hpp"

///////////////////////////////////////////////////////////////////////////////
// Forward declarations
///////////////////////////////////////////////////////////////////////////////

static void read_tty_output(Backlog_State* backlog, Pseudo_Terminal* tty, bool cap_read_calls);

static void tick_pipeline(Shell_State* shell,
                          Shell_Local* local,
                          Render_State* rend,
                          Prompt_State* prompt,
                          Backlog_State* backlog,
                          Running_Pipeline* pipeline,
                          Pseudo_Terminal* tty,
                          bool* force_quit);

static bool tick_program(Shell_State* shell,
                         Shell_Local* local,
                         Render_State* rend,
                         Prompt_State* prompt,
                         Backlog_State* backlog,
                         cz::Allocator allocator,
                         Running_Program* program,
                         Pseudo_Terminal* tty,
                         int* exit_code,
                         bool* force_quit);

///////////////////////////////////////////////////////////////////////////////
// Tick running node
///////////////////////////////////////////////////////////////////////////////

bool tick_running_node(Shell_State* shell,
                       Render_State* rend,
                       Prompt_State* prompt,
                       Running_Node* node,
                       Pseudo_Terminal* tty,
                       Backlog_State* backlog,
                       bool* force_quit) {
    for (size_t b = 0; b < node->bg.len; ++b) {
        Running_Pipeline* line = &node->bg[b];
        tick_pipeline(shell, node->local, rend, prompt, backlog, line, tty, force_quit);
        if (line->programs.len == 0) {
            finish_line(shell, *tty, node, backlog, line, /*background=*/true);
            --b;
        }
    }

    tick_pipeline(shell, node->local, rend, prompt, backlog, &node->fg, tty, force_quit);

    if (*force_quit)
        return true;

    read_tty_output(backlog, tty, /*cap_read_calls=*/true);

    if (node->fg.programs.len == 0 && !node->fg_finished) {
        bool started = finish_line(shell, *tty, node, backlog, &node->fg,
                                   /*background=*/false);
        if (!started)
            node->fg_finished = true;

        // Rerun to prevent long scripts from only doing one command per frame.
        // TODO: rate limit to prevent big scripts (with all builtins) from hanging.
        return true;
    }

    return false;
}

///////////////////////////////////////////////////////////////////////////////

static void tick_pipeline(Shell_State* shell,
                          Shell_Local* local,
                          Render_State* rend,
                          Prompt_State* prompt,
                          Backlog_State* backlog,
                          Running_Pipeline* pipeline,
                          Pseudo_Terminal* tty,
                          bool* force_quit) {
    cz::Allocator allocator = pipeline->arena.allocator();
    for (size_t p = 0; p < pipeline->programs.len; ++p) {
        Running_Program* program = &pipeline->programs[p];
        int exit_code = 1;
        if (tick_program(shell, local, rend, prompt, backlog, allocator, program, tty, &exit_code,
                         force_quit)) {
            if (!pipeline->has_exit_code && p + 1 == pipeline->programs.len) {
                pipeline->has_exit_code = true;
                pipeline->last_exit_code = exit_code;
            }
            backlog->end = std::chrono::steady_clock::now();
            pipeline->programs.remove(p);
            --p;
            if (pipeline->programs.len == 0)
                return;
        }
        if (*force_quit)
            return;
    }
}

///////////////////////////////////////////////////////////////////////////////

static void read_tty_output(Backlog_State* backlog, Pseudo_Terminal* tty, bool cap_read_calls) {
    static char buffer[4096];

#ifdef _WIN32
    cz::Input_File parent_out = tty->out;
#else
    cz::Input_File parent_out;
    parent_out.handle = tty->parent_bi;
#endif
    if (parent_out.is_open()) {
        int64_t result = 0;
        for (int rounds = 0;; ++rounds) {
            if (cap_read_calls && rounds == 1024)
                break;

#ifndef NDEBUG
            memset(buffer, 0xcd, sizeof(buffer));
#endif

            // Note: CRLF is stripped in append_text.
            result = parent_out.read(buffer, sizeof(buffer));
            if (result <= 0)
                break;

            // TODO: allow expanding max dynamically (don't close script->out here)
            result = append_text(backlog, {buffer, (size_t)result});
            if (result <= 0)
                break;
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
// Tick program
///////////////////////////////////////////////////////////////////////////////

static bool tick_program(Shell_State* shell,
                         Shell_Local* local,
                         Render_State* rend,
                         Prompt_State* prompt,
                         Backlog_State* backlog,
                         cz::Allocator allocator,
                         Running_Program* program,
                         Pseudo_Terminal* tty,
                         int* exit_code,
                         bool* force_quit) {
    switch (program->type) {
    case Running_Program::PROCESS:
        return program->v.process.try_join(exit_code);

    case Running_Program::SUB: {
        Running_Node* node = &program->v.sub;
        // TODO better rate limiting
        for (int rounds = 0; rounds < 128; ++rounds) {
            if (!tick_running_node(shell, rend, prompt, node, tty, backlog, force_quit)) {
                break;
            }
        }

        // TODO merge bg jobs up???
        if (node->fg_finished && node->bg.len == 0) {
            *exit_code = node->fg.last_exit_code;
            cleanup_local(node->local);
            cleanup_stdio(&node->stdio);
            return true;
        }
    } break;

    case Running_Program::ANY_BUILTIN:
        return tick_builtin(shell, local, rend, prompt, backlog, allocator, program, tty, exit_code,
                            force_quit);

    default:
        CZ_PANIC("unreachable");
    }
    return false;
}
