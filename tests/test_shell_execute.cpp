#include <czt/test_base.hpp>

#include "config.hpp"
#include "global.hpp"
#include "prompt.hpp"
#include "shell.hpp"

static cz::Buffer_Array temp_arena;
static Shell_State shell = {};
static Window_State window = {};
static Render_State rend = {};
static Prompt_State command_prompt = {};
static cz::Vector<Backlog_State*> backlogs = {};

struct Test_Info {
    cz::Str command;
    int expected_exit_code;
    cz::Str expected_output;
};
static cz::Vector<Test_Info> tests = {};

static void setup_environment() {
    temp_arena.init();
    temp_allocator = temp_arena.allocator();

    shell.arena.init();
    shell.width = 100;
    shell.height = 100;
    cfg.windows_wide_terminal = true;
    cfg.builtin_level = 2;

    rend.attached_outer = -1;
    rend.selected_outer = -1;
}

static void define_test(cz::Str command, int expected_exit_code, cz::Str expected_output) {
    Backlog_State* backlog = cz::heap_allocator().alloc<Backlog_State>();
    *backlog = {};
    init_backlog(backlog, backlogs.len, /*max_length=*/1ull << 30 /*1GB*/);
    backlog->refcount++;

    backlogs.reserve(cz::heap_allocator(), 1);
    backlogs.push(backlog);

    tests.reserve(cz::heap_allocator(), 1);
    tests.push({command, expected_exit_code, expected_output});

    bool result = run_script(&shell, backlog, command);
    if (!result) {
        backlog->exit_code = -1;
    }
}

static void wait_for_scripts_to_finish() {
    bool force_quit = false;
    while (!force_quit && shell.scripts.len > 0) {
        read_process_data(&shell, backlogs, &window, &rend, &command_prompt, &force_quit);
        SDL_Delay(1);
    }
}

TEST_CASE("execution tests") {
    setup_environment();

    define_test("file=$(mktemp) && echo hi | cat > $file && cat $file",  //
                /*exit_code=*/0, /*output=*/"hi\n");

    define_test("echo $(echo one | cat); echo $(echo two | cat) | cat",
                /*exit_code=*/0, /*output=*/"one\ntwo\n");

    define_test("a=1; c=1; (a=2; b=2; echo \"$a $b $c\"); echo \"$a $b $c\"\n",
                /*exit_code=*/0, /*output=*/"2 2 1\n1  1\n");

    define_test("a=1; c=1; (a=2; b=2; unset c; echo \"$a $b $c\"); echo \"$a $b $c\"\n",
                /*exit_code=*/0, /*output=*/"2 2 \n1  1\n");

    wait_for_scripts_to_finish();

    for (size_t i = 0; i < tests.len; ++i) {
        INFO("test index: " << i);
        INFO("command: " << tests[i].command.buffer);
        CHECK(backlogs[i]->exit_code == tests[i].expected_exit_code);
        CHECK(dbg_stringify_backlog(backlogs[i]) == tests[i].expected_output);
    }
}
