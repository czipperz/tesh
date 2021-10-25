#include "shell.hpp"

#include <cz/heap.hpp>

bool get_var(const Shell_State* shell, cz::Str key, cz::Str* value) {
    for (size_t i = 0; i < shell->variable_names.len; ++i) {
        if (key == shell->variable_names[i]) {
            *value = shell->variable_values[i];
            return true;
        }
    }

    return false;
}

void set_var(Shell_State* shell, cz::Str key, cz::Str value) {
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

void make_env_var(Shell_State* shell, cz::Str key) {
    for (size_t i = 0; i < shell->exported_vars.len; ++i) {
        if (key == shell->exported_vars[i])
            return;
    }

    shell->exported_vars.reserve(cz::heap_allocator(), 1);
    shell->exported_vars.push(key.clone(cz::heap_allocator()));
}

static void kill_program(Running_Program* program) {
    switch (program->type) {
    case Running_Program::PROCESS:
        program->v.process.kill();
        break;
    default:
        break;
    }
}

void cleanup_pipeline(Running_Pipeline* pipeline) {
    for (size_t p = 0; p < pipeline->pipeline.len; ++p) {
        kill_program(&pipeline->pipeline[p]);
    }
    for (size_t f = 0; f < pipeline->files.len; ++f) {
        pipeline->files[f].close();
    }
}

static void cleanup_process(Running_Script* script) {
    cleanup_pipeline(&script->fg.pipeline);

    script->in.close();
    script->out.close();
}

void cleanup_processes(Shell_State* shell) {
    for (size_t i = 0; i < shell->scripts.len; ++i) {
        Running_Script* script = &shell->scripts[i];
        cleanup_process(script);
    }
}

cz::Buffer_Array alloc_arena(Shell_State* shell) {
    if (shell->arenas.len > 0)
        return shell->arenas.pop();

    cz::Buffer_Array arena;
    arena.init();
    return arena;
}

void recycle_arena(Shell_State* shell, cz::Buffer_Array arena) {
    arena.clear();
    shell->arenas.reserve(cz::heap_allocator(), 1);
    shell->arenas.push(arena);
}

void recycle_pipeline(Shell_State* shell, Running_Pipeline* pipeline) {
    cleanup_pipeline(pipeline);

    // Empty pipelines don't have an arena.
    if (pipeline->length > 0)
        recycle_arena(shell, pipeline->arena);
}

void recycle_process(Shell_State* shell, Running_Script* script) {
    recycle_arena(shell, script->arena);

    // Empty pipelines don't have an arena.
    if (script->fg.pipeline.length > 0)
        recycle_arena(shell, script->fg.pipeline.arena);

    cleanup_process(script);

    if (script->id == shell->active_process)
        shell->active_process = -1;

    shell->scripts.remove(script - shell->scripts.elems);
}

Running_Script* active_process(Shell_State* shell) {
    if (shell->active_process == -1)
        return nullptr;

    for (size_t i = 0; i < shell->scripts.len; ++i) {
        Running_Script* script = &shell->scripts[i];
        if (script->id == shell->active_process)
            return script;
    }

    CZ_PANIC("Invalid active_process");
}
