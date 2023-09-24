#include "shell.hpp"

#include <cz/format.hpp>
#include <cz/heap.hpp>

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
        // TODO: free SET_VAR's value.
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

void cleanup_stdio(Stdio_State* stdio) {
    close_rc_file(stdio->in.count, stdio->in.file);
    close_rc_file(stdio->out.count, stdio->out.file);
    close_rc_file(stdio->err.count, stdio->err.file);
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

    shell->scripts.remove(script - shell->scripts.elems);
}

Running_Script* attached_process(Shell_State* shell, Render_State* rend) {
    if (rend->attached_outer == -1)
        return nullptr;
    Backlog_State* backlog = rend->visbacklogs[rend->attached_outer];
    return lookup_process(shell, backlog->id);
}
Running_Script* selected_process(Shell_State* shell, Render_State* rend) {
    if (rend->selected_outer == -1)
        return nullptr;
    Backlog_State* backlog = rend->visbacklogs[rend->selected_outer];
    return lookup_process(shell, backlog->id);
}

Running_Script* lookup_process(Shell_State* shell, uint64_t id) {
    for (size_t i = 0; i < shell->scripts.len; ++i) {
        Running_Script* script = &shell->scripts[i];
        if (script->id == id)
            return script;
    }
    return nullptr;
}

void append_parse_node(cz::Allocator allocator,
                       cz::String* string,
                       Parse_Node* node,
                       bool append_semicolon) {
    switch (node->type) {
    case Parse_Node::SEQUENCE: {
        if (node->async)
            cz::append(allocator, string, "(");

        for (size_t i = 0; i < node->v.sequence.len; ++i) {
            if (i > 0)
                cz::append(allocator, string, ' ');
            bool sub_append_semicolon = (i + 1 != node->v.sequence.len);
            append_parse_node(allocator, string, &node->v.sequence[i], sub_append_semicolon);
        }

        if (node->async)
            cz::append(allocator, string, ") &");
        else if (append_semicolon)
            cz::append(allocator, string, ";");
    } break;

    case Parse_Node::PROGRAM: {
        Parse_Program* program = node->v.program;

        for (size_t i = 0; i < program->variable_names.len; ++i) {
            if (i > 0)
                cz::append(allocator, string, ' ');
            cz::append(allocator, string, program->variable_names[i], '=',
                       program->variable_values[i]);
        }

        if (program->is_sub) {
            if (program->variable_names.len > 0)
                cz::append(allocator, string, ' ');
            cz::append(allocator, string, "(");
            append_parse_node(allocator, string, program->v.sub, false);
            cz::append(allocator, string, ")");
        } else {
            for (size_t i = 0; i < program->v.args.len; ++i) {
                if (i > 0 || program->variable_names.len > 0)
                    cz::append(allocator, string, ' ');
                cz::append(allocator, string, program->v.args[i]);
            }
        }

        if (program->in_file != "__tesh_std_in")
            cz::append(allocator, string, " < ", program->in_file);
        if (program->out_file != "__tesh_std_out") {
            if (program->out_file == "__tesh_std_err")
                cz::append(allocator, string, " >&2");
            else
                cz::append(allocator, string, " > ", program->out_file);
        }
        if (program->err_file != "__tesh_std_err") {
            if (program->err_file == "__tesh_std_out")
                cz::append(allocator, string, " 2>&1");
            else
                cz::append(allocator, string, " 2> ", program->err_file);
        }

        if (node->async)
            cz::append(allocator, string, " &");
        else if (append_semicolon)
            cz::append(allocator, string, ";");
    } break;

    case Parse_Node::PIPELINE: {
        if (node->async)
            cz::append(allocator, string, "(");

        for (size_t i = 0; i < node->v.pipeline.len; ++i) {
            if (i > 0)
                cz::append(allocator, string, " | ");

            if (node->v.pipeline[i].type == Parse_Node::SEQUENCE && !node->v.pipeline[i].async) {
                cz::append(allocator, string, "(");
                append_parse_node(allocator, string, &node->v.pipeline[i], false);
                cz::append(allocator, string, ")");
            } else {
                append_parse_node(allocator, string, &node->v.pipeline[i], false);
            }
        }

        if (node->async)
            cz::append(allocator, string, ") &");
        else if (append_semicolon)
            cz::append(allocator, string, ";");
    } break;

    case Parse_Node::AND:
    case Parse_Node::OR: {
        if (node->async)
            cz::append(allocator, string, "(");

        append_parse_node(allocator, string, node->v.binary.left, false);
        cz::append(allocator, string, (node->type == Parse_Node::AND ? " && " : " || "));
        append_parse_node(allocator, string, node->v.binary.right, false);

        if (node->async)
            cz::append(allocator, string, ") &");
        else if (append_semicolon)
            cz::append(allocator, string, ";");
    } break;

    case Parse_Node::IF: {
        cz::append(allocator, string, "if ");
        append_parse_node(allocator, string, node->v.if_.cond, true);
        cz::append(allocator, string, " then ");
        append_parse_node(allocator, string, node->v.if_.then, true);
        if (node->v.if_.other) {
            cz::append(allocator, string, " else ");
            append_parse_node(allocator, string, node->v.if_.other, true);
        }
        cz::append(allocator, string, " fi");

        if (node->async)
            cz::append(allocator, string, " &");
        else if (append_semicolon)
            cz::append(allocator, string, ";");
    } break;

    case Parse_Node::FUNCTION: {
        cz::append(allocator, string, node->v.function.name, "() { ");
        append_parse_node(allocator, string, node->v.function.body, true);
        cz::append(allocator, string, " }");

        if (node->async)
            cz::append(allocator, string, " &");
        else if (append_semicolon)
            cz::append(allocator, string, ";");
    } break;

    default:
        CZ_PANIC("Invalid Parse_Node type");
    }
}

const char* dbg_stringify_parse_node(Parse_Node* node) {
    cz::String string = {};
    append_parse_node(cz::heap_allocator(), &string, node, /*add_semicolon=*/false);
    string.reserve(cz::heap_allocator(), 1);
    string.realloc_null_terminate(cz::heap_allocator());
    return string.buffer;
}
