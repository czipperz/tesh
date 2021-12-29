#include "shell.hpp"

#include <cz/format.hpp>
#include <cz/heap.hpp>

static cz::Str canonical_var(cz::Str key) {
#ifdef _WIN32
    if (key == "PATH")
        return "Path";
#endif
    return key;
}

bool get_var(const Shell_Local* local, cz::Str key, cz::Str* value) {
    while (local && local->relationship == Shell_Local::ARGS_ONLY) {
        local = local->parent;
    }

    key = canonical_var(key);

    for (size_t i = 0; i < local->variable_names.len; ++i) {
        if (key == local->variable_names[i].str) {
            *value = local->variable_values[i].str;
            return true;
        }
    }

    return false;
}

void set_var(Shell_Local* local, cz::Str key, cz::Str value) {
    while (local && local->relationship == Shell_Local::ARGS_ONLY) {
        local = local->parent;
    }

    key = canonical_var(key);

    for (size_t i = 0; i < local->variable_names.len; ++i) {
        if (key == local->variable_names[i].str) {
            local->variable_values[i].drop();
            local->variable_values[i] = RcStr::create_clone(value);
            return;
        }
    }

    local->variable_names.reserve(cz::heap_allocator(), 1);
    local->variable_values.reserve(cz::heap_allocator(), 1);
    local->variable_names.push(RcStr::create_clone(key));
    local->variable_values.push(RcStr::create_clone(value));
}

cz::Str get_wd(const Shell_Local* local) {
    for (; local; local = local->parent) {
        if (local->working_directories.len > 0) {
            return local->working_directories.last();
        }
    }
    return "";
}
bool get_old_wd(const Shell_Local* local, size_t num, cz::Str* result) {
    for (; local; local = local->parent) {
        if (num >= local->working_directories.len) {
            num -= local->working_directories.len;
        } else {
            size_t index = local->working_directories.len - num - 1;
            *result = local->working_directories[index];
            return true;
        }
    }
    return false;
}
void set_wd(Shell_Local* local, cz::Str value) {
    while (local && local->relationship == Shell_Local::ARGS_ONLY) {
        local = local->parent;
    }

    // 128 should be enough.
    if (local->working_directories.len >= 128) {
        local->working_directories[0].drop(cz::heap_allocator());
        local->working_directories.remove(0);
    }

    local->working_directories.reserve(cz::heap_allocator(), 1);
    local->working_directories.push(value.clone_null_terminate(cz::heap_allocator()));
}

void make_env_var(Shell_Local* local, cz::Str key) {
    while (local && local->relationship == Shell_Local::ARGS_ONLY) {
        local = local->parent;
    }

    key = canonical_var(key);

    for (size_t i = 0; i < local->exported_vars.len; ++i) {
        if (key == local->exported_vars[i].str)
            return;
    }

    local->exported_vars.reserve(cz::heap_allocator(), 1);
    local->exported_vars.push(RcStr::create_clone(key));
}

void unset_var(Shell_Local* local, cz::Str key) {
    while (local && local->relationship == Shell_Local::ARGS_ONLY) {
        local = local->parent;
    }

    key = canonical_var(key);

    for (size_t i = 0; i < local->variable_names.len; ++i) {
        if (local->variable_names[i].str == key) {
            local->variable_names[i].drop();
            local->variable_names.remove(i);
            break;
        }
    }
    for (size_t i = 0; i < local->exported_vars.len; ++i) {
        if (local->exported_vars[i].str == key) {
            local->exported_vars[i].drop();
            local->exported_vars.remove(i);
            break;
        }
    }
}

int get_alias_or_function(const Shell_Local* local,
                          cz::Str alias_key,
                          cz::Str function_key,
                          Shell_Node** value) {
    bool allow_alias = true;
    for (const Shell_Local* elem = local; elem; elem = elem->parent) {
        if (alias_key == elem->blocked_alias) {
            allow_alias = false;
            break;
        }
    }

    for (; local; local = local->parent) {
        if (allow_alias) {
            for (size_t i = 0; i < local->alias_names.len; ++i) {
                if (local->alias_names[i] == alias_key) {
                    *value = local->alias_values[i];
                    return 1;
                }
            }
        }
        for (size_t i = 0; i < local->function_names.len; ++i) {
            if (local->function_names[i] == function_key) {
                *value = local->function_values[i];
                return 2;
            }
        }
    }
    return false;
}

void set_alias(Shell_Local* local, cz::Str key, Shell_Node* node) {
    while (local && local->relationship == Shell_Local::ARGS_ONLY) {
        local = local->parent;
    }

    for (size_t i = 0; i < local->alias_names.len; ++i) {
        if (local->alias_names[i] == key) {
            // TODO: deallocate old node.
            local->alias_values[i] = node;
            return;
        }
    }

    local->alias_names.reserve(cz::heap_allocator(), 1);
    local->alias_values.reserve(cz::heap_allocator(), 1);
    // TODO: garbage collect / ref count?
    local->alias_names.push(key.clone(cz::heap_allocator()));
    local->alias_values.push(node);
}

void set_function(Shell_Local* local, cz::Str key, Shell_Node* node) {
    while (local && local->relationship == Shell_Local::ARGS_ONLY) {
        local = local->parent;
    }

    for (size_t i = 0; i < local->function_names.len; ++i) {
        if (local->function_names[i] == key) {
            // TODO: deallocate old node.
            local->function_values[i] = node;
            return;
        }
    }

    local->function_names.reserve(cz::heap_allocator(), 1);
    local->function_values.reserve(cz::heap_allocator(), 1);
    // TODO: garbage collect / ref count?
    local->function_names.push(key.clone(cz::heap_allocator()));
    local->function_values.push(node);
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

static void cleanup_node(Running_Node* node) {
    for (size_t i = 0; i < node->bg.len; ++i) {
        cleanup_pipeline(&node->bg[i]);
    }
    cleanup_pipeline(&node->fg);

    // Don't cleanup persistent state (stdio and local) here because this
    // is ran at the end of every node instead of when the subshell exits.
}

static void kill_program(Running_Program* program) {
    switch (program->type) {
    case Running_Program::PROCESS: {
        program->v.process.kill();
    } break;

    case Running_Program::SUB: {
        Running_Node* node = &program->v.sub;
        cleanup_node(node);
        // Cleanup subshell.
        cleanup_stdio(&node->stdio);
        cleanup_local(node->local);
    } break;

    default: {
        // TODO: close CAT's file.
        cleanup_builtin(program);
    } break;
    }
}

void cleanup_pipeline(Running_Pipeline* pipeline) {
    for (size_t p = 0; p < pipeline->programs.len; ++p) {
        kill_program(&pipeline->programs[p]);
    }
    pipeline->has_exit_code = false;
    pipeline->last_exit_code = 0;
}

static void cleanup_script(Running_Script* script) {
    cleanup_node(&script->root);
    destroy_pseudo_terminal(&script->tty);
}

void cleanup_processes(Shell_State* shell) {
    for (size_t i = 0; i < shell->scripts.len; ++i) {
        Running_Script* script = &shell->scripts[i];
        cleanup_script(script);
    }
}

void cleanup_local(Shell_Local* local) {
    for (size_t i = 0; i < local->exported_vars.len; ++i) {
        local->exported_vars[i].drop();
    }
    for (size_t i = 0; i < local->variable_names.len; ++i) {
        local->variable_names[i].drop();
        local->variable_values[i].drop();
    }
    for (size_t i = 0; i < local->alias_names.len; ++i) {
        local->alias_names[i].drop(cz::heap_allocator());
    }
    for (size_t i = 0; i < local->function_names.len; ++i) {
        local->function_names[i].drop(cz::heap_allocator());
    }
    for (size_t i = 0; i < local->working_directories.len; ++i) {
        local->working_directories[i].drop(cz::heap_allocator());
    }

    local->variable_names.drop(cz::heap_allocator());
    local->variable_values.drop(cz::heap_allocator());
    local->alias_names.drop(cz::heap_allocator());
    local->alias_values.drop(cz::heap_allocator());
    local->working_directories.drop(cz::heap_allocator());
}

void cleanup_stdio(Stdio_State* stdio) {
    close_rc_file(stdio->in_count, stdio->in);
    close_rc_file(stdio->out_count, stdio->out);
    close_rc_file(stdio->err_count, stdio->err);
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

void append_node(cz::Allocator allocator,
                 cz::String* string,
                 Shell_Node* node,
                 bool append_semicolon) {
    switch (node->type) {
    case Shell_Node::SEQUENCE: {
        if (node->async)
            cz::append(allocator, string, "(");

        for (size_t i = 0; i < node->v.sequence.len; ++i) {
            if (i > 0)
                cz::append(allocator, string, ' ');
            append_node(allocator, string, &node->v.sequence[i], true);
        }

        if (node->async)
            cz::append(allocator, string, ") &");
        else if (append_semicolon)
            cz::append(allocator, string, ";");
    } break;

    case Shell_Node::PROGRAM: {
        Parse_Program* program = node->v.program;

        for (size_t i = 0; i < program->subexprs.len; ++i) {
            if (i > 0)
                cz::append(allocator, string, ' ');
            cz::append(allocator, string, "__tesh_sub", i, "=$(");
            append_node(allocator, string, program->subexprs[i], false);
            cz::append(allocator, string, ")");
        }

        for (size_t i = 0; i < program->variable_names.len; ++i) {
            if (i > 0 || program->subexprs.len > 0)
                cz::append(allocator, string, ' ');
            cz::append(allocator, string, program->variable_names[i], '=',
                       program->variable_values[i]);
        }

        if (program->is_sub) {
            if (program->variable_names.len > 0 || program->subexprs.len > 0)
                cz::append(allocator, string, ' ');
            cz::append(allocator, string, "(");
            append_node(allocator, string, program->v.sub, false);
            cz::append(allocator, string, ")");
        } else {
            for (size_t i = 0; i < program->v.args.len; ++i) {
                if (i > 0 || program->variable_names.len > 0 || program->subexprs.len > 0)
                    cz::append(allocator, string, ' ');
                cz::append(allocator, string, program->v.args[i]);
            }
        }

        if (program->in_file.buffer)
            cz::append(allocator, string, " < ", program->in_file);
        if (program->out_file.buffer)
            cz::append(allocator, string, " > ", program->out_file);
        if (program->err_file.buffer)
            cz::append(allocator, string, " 2> ", program->err_file);

        if (node->async)
            cz::append(allocator, string, " &");
        else if (append_semicolon)
            cz::append(allocator, string, ";");
    } break;

    case Shell_Node::PIPELINE: {
        if (node->async)
            cz::append(allocator, string, "(");

        for (size_t i = 0; i < node->v.pipeline.len; ++i) {
            if (i > 0)
                cz::append(allocator, string, " | ");
            append_node(allocator, string, &node->v.pipeline[i], false);
        }

        if (node->async)
            cz::append(allocator, string, ") &");
        else if (append_semicolon)
            cz::append(allocator, string, ";");
    } break;

    case Shell_Node::AND:
    case Shell_Node::OR: {
        if (node->async)
            cz::append(allocator, string, "(");

        append_node(allocator, string, node->v.binary.left, false);
        cz::append(allocator, string, (node->type == Shell_Node::AND ? " && " : " || "));
        append_node(allocator, string, node->v.binary.right, false);

        if (node->async)
            cz::append(allocator, string, ") &");
        else if (append_semicolon)
            cz::append(allocator, string, ";");
    } break;

    case Shell_Node::IF: {
        cz::append(allocator, string, "if ");
        append_node(allocator, string, node->v.if_.cond, true);
        cz::append(allocator, string, " then ");
        append_node(allocator, string, node->v.if_.then, true);
        if (node->v.if_.other) {
            cz::append(allocator, string, " else ");
            append_node(allocator, string, node->v.if_.other, true);
        }
        cz::append(allocator, string, " fi");

        if (node->async)
            cz::append(allocator, string, " &");
        else if (append_semicolon)
            cz::append(allocator, string, ";");
    } break;

    case Shell_Node::FUNCTION: {
        cz::append(allocator, string, node->v.function.name, "() { ");
        append_node(allocator, string, node->v.function.body, true);
        cz::append(allocator, string, " }");

        if (node->async)
            cz::append(allocator, string, " &");
        else if (append_semicolon)
            cz::append(allocator, string, ";");
    } break;

    default:
        CZ_PANIC("Invalid Shell_Node type");
    }
}
