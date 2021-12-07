#include "shell.hpp"

#include <cz/heap.hpp>

static cz::Str canonical_var(cz::Str key) {
#ifdef _WIN32
    if (key == "PATH")
        return "Path";
#endif
    return key;
}

bool get_var(const Shell_Local* local, cz::Str key, cz::Str* value) {
    key = canonical_var(key);

    for (; local; local = local->parent) {
        for (size_t i = 0; i < local->variable_names.len; ++i) {
            if (key == local->variable_names[i]) {
                *value = local->variable_values[i];
                return true;
            }
        }
    }

    return false;
}

void set_var(Shell_Local* local, cz::Str key, cz::Str value) {
    key = canonical_var(key);

    for (size_t i = 0; i < local->variable_names.len; ++i) {
        if (key == local->variable_names[i]) {
            local->variable_values[i].drop(cz::heap_allocator());
            local->variable_values[i] = value.clone_null_terminate(cz::heap_allocator());
            return;
        }
    }

    local->variable_names.reserve(cz::heap_allocator(), 1);
    local->variable_values.reserve(cz::heap_allocator(), 1);
    local->variable_names.push(key.clone(cz::heap_allocator()));
    local->variable_values.push(value.clone_null_terminate(cz::heap_allocator()));
}

#define SPECIAL_WD_VAR "__tesh_wd"

cz::Str get_wd(const Shell_Local* local) {
    cz::Str wd;
    if (get_var(local, SPECIAL_WD_VAR, &wd))
        return wd;
    return "";
}
void set_wd(Shell_Local* local, cz::Str value) {
    set_var(local, SPECIAL_WD_VAR, value);
}

void make_env_var(Shell_State* shell, cz::Str key) {
    key = canonical_var(key);

    for (size_t i = 0; i < shell->exported_vars.len; ++i) {
        if (key == shell->exported_vars[i])
            return;
    }

    shell->exported_vars.reserve(cz::heap_allocator(), 1);
    shell->exported_vars.push(key.clone(cz::heap_allocator()));
}

bool get_alias(const Shell_Local* local, cz::Str key, cz::Str* value) {
    for (; local; local = local->parent) {
        for (size_t i = 0; i < local->alias_names.len; ++i) {
            if (local->alias_names[i] == key) {
                *value = local->alias_values[i];
                return true;
            }
        }
    }
    return false;
}

void set_alias(Shell_Local* local, cz::Str key, cz::Str value) {
    for (size_t i = 0; i < local->alias_names.len; ++i) {
        if (local->alias_names[i] == key) {
            cz::Str* slot = &local->alias_values[i];
            cz::heap_allocator().dealloc({(void*)slot->buffer, slot->len});
            *slot = value.clone(cz::heap_allocator());
        }
    }

    local->alias_names.reserve(cz::heap_allocator(), 1);
    local->alias_values.reserve(cz::heap_allocator(), 1);
    // TODO: garbage collect / ref count?
    local->alias_names.push(key.clone(cz::heap_allocator()));
    local->alias_values.push(value.clone(cz::heap_allocator()));
}

void close_rc_file(size_t* count, cz::File_Descriptor file) {
    if (count) {
        --*count;
        if (*count == 0) {
            file.close();
        }
    }
}

void cleanup_builtin(Running_Program* program) {
    auto& builtin = program->v.builtin;
    close_rc_file(builtin.in_count, builtin.in.file);
    if (builtin.out.type == Process_Output::FILE)
        close_rc_file(builtin.out_count, builtin.out.v.file);
    if (builtin.err.type == Process_Output::FILE)
        close_rc_file(builtin.err_count, builtin.err.v.file);
}

static void kill_program(Running_Program* program) {
    switch (program->type) {
    case Running_Program::PROCESS:
        program->v.process.kill();
        break;
    default:
        // TODO: close CAT's file.
        cleanup_builtin(program);
        break;
    }
}

void cleanup_pipeline(Running_Pipeline* pipeline) {
    for (size_t p = 0; p < pipeline->programs.len; ++p) {
        kill_program(&pipeline->programs[p]);
    }
}

static void cleanup_script(Running_Script* script) {
    for (size_t i = 0; i < script->root.bg.len; ++i) {
        cleanup_pipeline(&script->root.bg[i]);
    }
    cleanup_pipeline(&script->root.fg);
    destroy_pseudo_terminal(&script->tty);
}

void cleanup_processes(Shell_State* shell) {
    for (size_t i = 0; i < shell->scripts.len; ++i) {
        Running_Script* script = &shell->scripts[i];
        cleanup_script(script);
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
    recycle_arena(shell, pipeline->arena);
}

void recycle_process(Shell_State* shell, Running_Script* script) {
    cleanup_script(script);

    recycle_arena(shell, script->arena);

    for (size_t i = 0; i < script->root.bg.len; ++i)
        recycle_arena(shell, script->root.bg[i].arena);

    if (!script->root.fg_finished)
        recycle_arena(shell, script->root.fg.arena);

    if (script->id == shell->attached_process)
        shell->attached_process = -1;

    shell->scripts.remove(script - shell->scripts.elems);
}

Running_Script* attached_process(Shell_State* shell) {
    if (shell->attached_process == -1)
        return nullptr;
    return lookup_process(shell, shell->attached_process);
}
Running_Script* selected_process(Shell_State* shell) {
    if (shell->selected_process == -1)
        return nullptr;
    return lookup_process(shell, shell->selected_process);
}

Running_Script* lookup_process(Shell_State* shell, uint64_t id) {
    for (size_t i = 0; i < shell->scripts.len; ++i) {
        Running_Script* script = &shell->scripts[i];
        if (script->id == id)
            return script;
    }
    return nullptr;
}
