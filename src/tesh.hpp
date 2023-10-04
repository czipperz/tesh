#pragma once

#include <cz/vector.hpp>
#include "backlog.hpp"
#include "prompt.hpp"
#include "render.hpp"
#include "search.hpp"
#include "shell.hpp"

struct Pane_State {
    Render_State rend;
    cz::Vector<Backlog_State*> backlogs;
    Prompt_State command_prompt;
    Search_State search;
    Shell_State shell;

    void init();
    void drop();
};

struct Tesh_State {
    Window_State window;
    cz::Vector<Pane_State*> panes;
    size_t selected_pane;
};
