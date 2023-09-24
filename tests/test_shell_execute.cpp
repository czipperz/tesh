#include <czt/test_base.hpp>

#include "config.hpp"
#include "global.hpp"
#include "prompt.hpp"
#include "shell.hpp"

static cz::Buffer_Array temp_arena;
static Shell_State shell = {};
static Render_State rend = {};
static Prompt_State command_prompt = {};
static cz::Vector<Backlog_State*> backlogs = {};

static void setup_environment() {
    temp_arena.init();
    temp_allocator = temp_arena.allocator();

    shell.width = 100;
    shell.height = 100;
    cfg.windows_wide_terminal = true;
    cfg.builtin_level = 2;

    rend.attached_outer = -1;
    rend.selected_outer = -1;
}

static void execute(cz::Str command) {
    Backlog_State* backlog = cz::heap_allocator().alloc<Backlog_State>();
    *backlog = {};
    init_backlog(backlog, backlogs.len, /*max_length=*/1ull << 30 /*1GB*/);
    backlog->refcount++;

    backlogs.reserve(cz::heap_allocator(), 1);
    backlogs.push(backlog);

    bool result = run_script(&shell, backlog, command);
    if (!result) {
        backlog->exit_code = -1;
    }
}

static void wait_for_scripts_to_finish() {
    bool force_quit = false;
    while (!force_quit && shell.scripts.len > 0) {
        read_process_data(&shell, backlogs, &rend, &command_prompt, &force_quit);
        SDL_Delay(1);
    }
}

TEST_CASE("execution tests") {
    setup_environment();

    bool first = true;

top:
    size_t i = 0;

    if (first) {
        execute("file=$(mktemp) && echo hi | cat > $file && cat $file");
    } else {
        CHECK(backlogs[i]->exit_code == 0);
        CHECK(dbg_stringify_backlog(backlogs[i]) == "hi\n");
        ++i;
    }

    if (first) {
        execute("echo $(echo one | cat); echo $(echo two | cat) | cat");
    } else {
        CHECK(backlogs[i]->exit_code == 0);
        CHECK(dbg_stringify_backlog(backlogs[i]) == "one\ntwo\n");
        ++i;
    }

    if (first) {
        execute("a=1; c=1; (a=2; b=2; echo \"$a $b $c\"); echo \"$a $b $c\"\n");
    } else {
        CHECK(backlogs[i]->exit_code == 0);
        CHECK(dbg_stringify_backlog(backlogs[i]) == "2 2 1\n1  1\n");
        ++i;
    }

    if (first) {
        execute("a=1; c=1; (a=2; b=2; unset c; echo \"$a $b $c\"); echo \"$a $b $c\"\n");
    } else {
        CHECK(backlogs[i]->exit_code == 0);
        CHECK(dbg_stringify_backlog(backlogs[i]) == "2 2 \n1  1\n");
        ++i;
    }

    if (!first)
        return;

    wait_for_scripts_to_finish();

    first = false;
    goto top;
}
