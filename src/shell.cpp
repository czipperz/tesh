#include "shell.hpp"

#include <cz/heap.hpp>

bool get_env_var(const Shell_State* shell, cz::Str key, cz::Str* value) {
    for (size_t i = 0; i < shell->variable_names.len; ++i) {
        if (key == shell->variable_names[i]) {
            *value = shell->variable_values[i];
            return true;
        }
    }

    return false;
}

void set_env_var(Shell_State* shell, cz::Str key, cz::Str value) {
    for (size_t i = 0; i < shell->variable_names.len; ++i) {
        if (key == shell->variable_names[i]) {
            shell->variable_values[i].drop(cz::heap_allocator());
            shell->variable_values[i] = value.clone(cz::heap_allocator());
            return;
        }
    }

    shell->variable_names.reserve(cz::heap_allocator(), 1);
    shell->variable_values.reserve(cz::heap_allocator(), 1);
    shell->variable_names.push(key.clone(cz::heap_allocator()));
    shell->variable_values.push(value.clone(cz::heap_allocator()));
}

void cleanup_processes(Shell_State* shell) {
    for (size_t i = 0; i < shell->lines.len; ++i) {
        Running_Line* line = &shell->lines[i];
        // TODO: cleanup fds
        for (size_t i = 0; i < line->pipeline.len; ++i) {
            Running_Program* program = &line->pipeline[i];
            switch (program->type) {
            case Running_Program::PROCESS:
                program->v.process.kill();
                break;
            }
        }
    }
}
