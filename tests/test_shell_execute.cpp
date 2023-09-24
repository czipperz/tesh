#include <czt/test_base.hpp>

#include "config.hpp"
#include "global.hpp"
#include "prompt.hpp"
#include "shell.hpp"

static Backlog_State execute(cz::Str command) {
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

    bool result = run_script(&shell, &backlog, command);
    if (!result) {
        backlog.exit_code = -1;
        return backlog;
    }

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

    return backlog;
}

TEST_CASE("execute: file=$(mktemp) && echo hi | cat > $file && cat $file") {
    Backlog_State backlog = execute("file=$(mktemp) && echo hi | cat > $file && cat $file");
    CHECK(backlog.exit_code == 0);
    CHECK(dbg_stringify_backlog(&backlog) == "hi\n");
}

TEST_CASE("execute: pipe + $() combos") {
    Backlog_State backlog = execute("echo $(echo one | cat); echo $(echo two | cat) | cat");
    CHECK(backlog.exit_code == 0);
    CHECK(dbg_stringify_backlog(&backlog) == "one\ntwo\n");
}
