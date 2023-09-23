#include <czt/test_base.hpp>

#include "config.hpp"
#include "global.hpp"
#include "shell.hpp"
#include "prompt.hpp"

TEST_CASE("execute: echo hi") {
    cz::Buffer_Array temp_arena;
    temp_arena.init();
    temp_allocator = temp_arena.allocator();

    Shell_State shell = {};
    shell.width = 100;
    shell.height = 100;
    cfg.windows_wide_terminal = true;
    cfg.builtin_level = 2;

    Backlog_State backlog = {};
    init_backlog(&backlog, /*id=*/0, /*max_length=*/1ull << 30 /*1GB*/);
    backlog.refcount++;

    bool result = run_script(&shell, &backlog, "echo hi");
    CHECK(result);

    Render_State rend = {};
    rend.attached_outer = -1;
    rend.selected_outer = -1;

    Prompt_State command_prompt = {};

    bool force_quit = false;
    while (!force_quit && shell.scripts.len > 0) {
        Backlog_State* backlogs[] = {&backlog};
        read_process_data(&shell, backlogs, &rend, &command_prompt, &force_quit);
        SDL_Delay(1);
    }

    CHECK(backlog.exit_code == 0);
    CHECK(dbg_stringify_backlog(&backlog) == "hi\n");
}
