#include "tesh.hpp"

////////////////////////////////////////////////////////////////////////////////
// Pane lifecycle
////////////////////////////////////////////////////////////////////////////////

void Pane_State::init() {
    shell.arena.init();
    command_prompt.init();
    search.prompt.init();

    command_prompt.prefix = " $ ";
    search.prompt.prefix = "SEARCH> ";
    rend.complete_redraw = true;
}

void Pane_State::drop() {
    cleanup_processes(&shell);
}
