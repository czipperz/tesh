#include "shell.hpp"

#include <cz/debug.hpp>
#include <cz/defer.hpp>
#include <cz/format.hpp>
#include <cz/heap.hpp>
#include <cz/path.hpp>
#include <cz/process.hpp>
#include <tracy/Tracy.hpp>

#include "config.hpp"
#include "global.hpp"

///////////////////////////////////////////////////////////////////////////////
// Forward declarations
///////////////////////////////////////////////////////////////////////////////

static Error start_execute_line(Shell_State* shell,
                                const Pseudo_Terminal& tty,
                                Running_Node* node,
                                Backlog_State* backlog,
                                Running_Pipeline* pipeline,
                                bool background);

static void start_execute_pipeline(Shell_State* shell,
                                   const Pseudo_Terminal& tty,
                                   Running_Node* node,
                                   Backlog_State* backlog,
                                   Running_Pipeline* pipeline,
                                   bool bind_stdin);

static void expand_file_argument(cz::Str* path, Shell_Local* local, cz::Allocator allocator);
static Error link_stdio(Stdio_State* stdio,
                        cz::Input_File* pipe_in,
                        const Parse_Program* parse_program,
                        cz::Allocator allocator,
                        cz::Slice<Parse_Node> program_nodes,
                        size_t p,
                        bool bind_stdin);
static void open_redirected_files(Stdio_State* stdio,
                                  cz::Str* error_path,
                                  const Parse_Program& parse_program,
                                  cz::Allocator allocator,
                                  Shell_Local* local);

static Error run_program(Shell_State* shell,
                         Shell_Local* local,
                         cz::Allocator allocator,
                         const Pseudo_Terminal& tty,
                         Running_Program* program,
                         Parse_Program parse,
                         Stdio_State stdio,
                         Backlog_State* backlog,
                         cz::Str error_path);

static void print_error(Output_Object& out, Backlog_State* backlog, Error error);

///////////////////////////////////////////////////////////////////////////////

enum Walk_Status {
    WALK_FAILURE = 0,
    WALK_SUCCESS = 1,
    WALK_ASYNC = 2,
};

static bool descend_to_first_pipeline(cz::Vector<Parse_Node*>* path, Parse_Node* child);
static void do_descend_to_first_pipeline(cz::Vector<Parse_Node*>* path, Parse_Node* child);
static bool walk_to_next_pipeline(cz::Vector<Parse_Node*>* path, Walk_Status status);

///////////////////////////////////////////////////////////////////////////////
// Initialization
///////////////////////////////////////////////////////////////////////////////

static cz::Input_File null_input;
static cz::Output_File null_output;
static size_t null_input_count = 1;
static size_t null_output_count = 1;

void create_null_file() {
#ifdef _WIN32
    const char* NULL_FILE = "NUL";
#else
    const char* NULL_FILE = "/dev/null";
#endif

    bool succ1 = null_input.open(NULL_FILE);
    CZ_ASSERT(succ1);
    bool succ2 = null_output.open(NULL_FILE);
    CZ_ASSERT(succ2);
}

///////////////////////////////////////////////////////////////////////////////
// Start executing a script
///////////////////////////////////////////////////////////////////////////////

bool run_script(Shell_State* shell, Backlog_State* backlog, cz::Str command) {
    ZoneScoped;

#ifdef TRACY_ENABLE
    {
        cz::String message = cz::format(temp_allocator, "Start: ", command);
        TracyMessage(message.buffer, message.len);
    }
#endif

    cz::Buffer_Array arena = alloc_arena(shell);

    // Root has to be kept alive for path traversal to work.
    Parse_Node* root = arena.allocator().alloc<Parse_Node>();
    *root = {};

    cz::String text = command.clone_null_terminate(arena.allocator());
    Error error = parse_script(shell->arena.allocator(), arena.allocator(), root, text);
    if (error != Error_Success)
        goto fail;

    error = start_execute_script(shell, backlog, arena, root);
    if (error != Error_Success)
        goto fail;

    return true;

fail:;
#ifdef TRACY_ENABLE
    {
        cz::String message = cz::format(temp_allocator, "Failed to start: ", command);
        TracyMessage(message.buffer, message.len);
    }
#endif

    append_text(backlog, "tesh: Error: ");
    append_text(backlog, error_string(error));
    append_text(backlog, "\n");
    backlog->exit_code = -1;
    backlog->done = true;
    backlog->end = std::chrono::steady_clock::now();
    // Decrement refcount in caller.

    recycle_arena(shell, arena);
    return false;
}

Error start_execute_script(Shell_State* shell,
                           Backlog_State* backlog,
                           cz::Buffer_Array arena,
                           Parse_Node* root) {
    ZoneScoped;

    Running_Script running = {};
    running.id = backlog->id;
    running.arena = arena;

    if (!create_pseudo_terminal(&running.tty, shell->width, shell->height))
        return Error_IO;

    running.parse_root = root;

    running.root.local = &shell->local;

#ifdef _WIN32
    running.root.stdio.in.file = running.tty.child_in;
    running.root.stdio.out.file = running.tty.child_out;
#else
    running.root.stdio.in.file.handle = running.tty.child_bi;
    running.root.stdio.out.file.handle = running.tty.child_bi;
#endif
    running.root.stdio.err.file = running.root.stdio.out.file;  // stderr = stdout at top level

    Error error = start_execute_node(shell, running.tty, backlog, &running.root, root);
    if (error != Error_Success) {
        destroy_pseudo_terminal(&running.tty);
        return error;
    }

    shell->scripts.reserve(cz::heap_allocator(), 1);
    shell->scripts.push(running);
    return Error_Success;
}

Error start_execute_node(Shell_State* shell,
                         const Pseudo_Terminal& tty,
                         Backlog_State* backlog,
                         Running_Node* node,
                         Parse_Node* root) {
    ZoneScoped;

    node->fg.arena = alloc_arena(shell);

    if (!descend_to_first_pipeline(&node->fg.path, root))
        return Error_Success;

    const bool background = false;
    Error error = start_execute_line(shell, tty, node, backlog, &node->fg, background);
    if (error != Error_Success) {
        recycle_arena(shell, node->fg.arena);
    }
    return error;
}

///////////////////////////////////////////////////////////////////////////////
// Finish a line in the script
///////////////////////////////////////////////////////////////////////////////

bool finish_line(Shell_State* shell,
                 const Pseudo_Terminal& tty,
                 Running_Node* node,
                 Backlog_State* backlog,
                 Running_Pipeline* line,
                 bool background) {
    ZoneScoped;

#if defined(TRACY_ENABLE) && 0
    {
        cz::String message = cz::format(temp_allocator, "End: ", line->pipeline.command_line);
        TracyMessage(message.buffer, message.len);
    }
#endif

    Walk_Status status = (line->last_exit_code == 0 ? WALK_SUCCESS : WALK_FAILURE);
    bool has_next = walk_to_next_pipeline(&line->path, status);
    if (!has_next) {
        if (!background)
            backlog->exit_code = line->last_exit_code;
        recycle_pipeline(shell, line);
        if (background) {
            node->bg.remove(line - node->bg.elems);
        } else {
            node->fg_finished = true;
        }
        return false;
    }

    cleanup_pipeline(line);
    Error error = start_execute_line(shell, tty, node, backlog, line, background);
    if (error != Error_Success) {
        print_error(node->stdio.err, backlog, error);
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// Path walking
///////////////////////////////////////////////////////////////////////////////

static bool descend_to_first_pipeline(cz::Vector<Parse_Node*>* path, Parse_Node* child) {
    ZoneScoped;

    do_descend_to_first_pipeline(path, child);
    if (path->last() != nullptr)
        return true;

    path->pop();
    return walk_to_next_pipeline(path, WALK_SUCCESS);
}

static void do_descend_to_first_pipeline(cz::Vector<Parse_Node*>* path, Parse_Node* child) {
    while (1) {
        path->reserve(cz::heap_allocator(), 1);
        path->push(child);

        switch (child->type) {
        case Parse_Node::PROGRAM:
        case Parse_Node::PIPELINE:
        case Parse_Node::FUNCTION:
            return;
        case Parse_Node::SEQUENCE:
            if (child->v.sequence.len == 0) {
                path->reserve(cz::heap_allocator(), 1);
                path->push(nullptr);
                return;
            } else {
                child = &child->v.sequence[0];
            }
            break;
        case Parse_Node::AND:
        case Parse_Node::OR:
            child = child->v.binary.left;
            break;
        case Parse_Node::IF:
            child = child->v.if_.cond;
            break;
        default:
            CZ_PANIC("Invalid Parse_Node type");
        }
    }
}

static bool walk_to_next_pipeline(cz::Vector<Parse_Node*>* path, Walk_Status status) {
    if (status == WALK_ASYNC)
        CZ_DEBUG_ASSERT(path->len > 0 && path->last()->async);
    bool success = (status != WALK_FAILURE);

    while (1) {
        if (path->len < 2) {
            path->len = 0;
            return false;
        }

        Parse_Node* child = path->pop();
        if (child->async) {
            if (status == WALK_ASYNC) {
                status = WALK_SUCCESS;
            } else {
                return false;
            }
        }

        Parse_Node* parent = path->last();
        switch (parent->type) {
        case Parse_Node::SEQUENCE: {
            size_t i = 0;
            for (; i < parent->v.sequence.len; ++i) {
                if (child == &parent->v.sequence[i]) {
                    ++i;
                    break;
                }
            }
            if (i < parent->v.sequence.len)
                return descend_to_first_pipeline(path, &parent->v.sequence[i]);
        } break;

        case Parse_Node::AND: {
            if (child == parent->v.binary.left && success)
                return descend_to_first_pipeline(path, parent->v.binary.right);
        } break;

        case Parse_Node::OR: {
            if (child == parent->v.binary.left && !success)
                return descend_to_first_pipeline(path, parent->v.binary.right);
        } break;

        case Parse_Node::IF: {
            if (child == parent->v.if_.cond) {
                if (success)
                    return descend_to_first_pipeline(path, parent->v.if_.then);
                else if (parent->v.if_.other)
                    return descend_to_first_pipeline(path, parent->v.if_.other);
            }
        } break;

        case Parse_Node::PROGRAM:
        case Parse_Node::PIPELINE:
        case Parse_Node::FUNCTION:
            CZ_PANIC("invalid");

        default:
            CZ_PANIC("Invalid Parse_Node type");
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
// Start execute line
///////////////////////////////////////////////////////////////////////////////

static Error start_execute_line(Shell_State* shell,
                                const Pseudo_Terminal& tty,
                                Running_Node* node,
                                Backlog_State* backlog,
                                Running_Pipeline* pipeline,
                                bool background) {
    ZoneScoped;

    // Don't do this on Windows to avoid a race condition because the pipe gets flushed post exit.
#ifndef _WIN32
    if (backlog->length > 0 && backlog->get(backlog->length - 1) != '\n') {
        append_text(backlog, "\n");
    }
#endif

    Running_Pipeline* pipeline_orig = pipeline;
    while (1) {
        pipeline = pipeline_orig;
        bool async = pipeline->path.last()->async;
        if (async) {
            Running_Pipeline line = {};
            line.arena = alloc_arena(shell);
            line.path = pipeline->path.clone(cz::heap_allocator());
            node->bg.reserve(cz::heap_allocator(), 1);
            node->bg.push(line);
            pipeline = &node->bg.last();
        }

        bool bind_stdin = !(background || async);
        start_execute_pipeline(shell, tty, node, backlog, pipeline, bind_stdin);

        if (!async) {
            return Error_Success;
        }

        // Find the next node to execute.
        if (!walk_to_next_pipeline(&pipeline_orig->path, WALK_ASYNC)) {
            // No node so stop.
            recycle_pipeline(shell, pipeline_orig);
            if (background) {
                node->bg.remove(pipeline_orig - node->bg.elems);
            } else {
                node->fg_finished = true;
            }
            return Error_Success;
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
// Start execute pipeline
///////////////////////////////////////////////////////////////////////////////

static void start_execute_pipeline(Shell_State* shell,
                                   const Pseudo_Terminal& tty,
                                   Running_Node* node,
                                   Backlog_State* backlog,
                                   Running_Pipeline* pipeline,
                                   bool bind_stdin) {
    ZoneScoped;

    // TODO async stuff inside our children needs to be allocated separately.
    pipeline->arena.clear();

    // Get the nodes in the pipeline.
    Parse_Node* pipeline_node = pipeline->path.last();
    cz::Slice<Parse_Node> program_nodes;
    if (pipeline_node->type == Parse_Node::PIPELINE) {
        program_nodes = pipeline_node->v.pipeline;
    } else {
        // Only one element in the pipeline so it's left raw.
        program_nodes = {pipeline_node, 1};
    }

    cz::Allocator allocator = pipeline->arena.allocator();

    cz::Vector<Running_Program> programs = {};
    CZ_DEFER(programs.drop(cz::heap_allocator()));
    cz::Input_File pipe_in;

    for (size_t p = 0; p < program_nodes.len; ++p) {
        Parse_Node* program_node = &program_nodes[p];

        Running_Program running_program = {};

        if (program_node->type == Parse_Node::FUNCTION) {
            // Declare the function and ignore all pipe and file indirection.
            set_function(node->local, program_node->v.function.name, program_node->v.function.body);
            continue;
        } else if (program_node->type == Parse_Node::PROGRAM) {
#ifdef TRACY_ENABLE
            cz::String message = cz::format(temp_allocator, "run program: ");
            append_parse_node(temp_allocator, &message, program_node, /*add_semicolon=*/false);
            TracyMessage(message.buffer, message.len);
#endif

            // Normal case for example `echo` and `cat` in `echo | cat`.
            Parse_Program parse_program = *program_node->v.program;

            expand_file_argument(&parse_program.in_file, node->local, allocator);
            expand_file_argument(&parse_program.out_file, node->local, allocator);
            expand_file_argument(&parse_program.err_file, node->local, allocator);

            Stdio_State stdio = node->stdio;
            Error error = link_stdio(&stdio, &pipe_in, &parse_program, allocator, program_nodes, p,
                                     bind_stdin);
            if (error != Error_Success) {
                print_error(node->stdio.err, backlog, error);
                return;
            }

            cz::Str error_path = {};
            open_redirected_files(&stdio, &error_path, parse_program, allocator, node->local);

            error = run_program(shell, node->local, allocator, tty, &running_program, parse_program,
                                stdio, backlog, error_path);
            if (error != Error_Success) {
                print_error(node->stdio.err, backlog, error);
                return;
            }
        } else {
            // Compound pipeline + subnode.  For example:
            // `(echo hi; cat file)` in `(echo hi; cat file) | grep x`.
            Stdio_State stdio = node->stdio;
            Error error = link_stdio(&stdio, &pipe_in, /*parse_program=*/nullptr, allocator,
                                     program_nodes, p, bind_stdin);
            if (error != Error_Success) {
                print_error(node->stdio.err, backlog, error);
                return;
            }

            running_program.type = Running_Program::SUB;
            running_program.v.sub = build_sub_running_node(node->local, stdio, allocator);

            error = start_execute_node(shell, tty, backlog, &running_program.v.sub, program_node);
            if (error != Error_Success) {
                print_error(node->stdio.err, backlog, error);
                return;
            }
        }

        programs.reserve(cz::heap_allocator(), 1);
        programs.push(running_program);
    }

    pipeline->programs = programs.clone(allocator);
}

/// Build a subnode.  We need a `Running_Node` in order to execute a `Parse_Node`.
/// By default, this just copies the parent's environment.  Most callers will edit
/// the created `Shell_Local` to change the spawned environment.
Running_Node build_sub_running_node(Shell_Local* parent_local,
                                    const Stdio_State& stdio,
                                    cz::Allocator allocator) {
    Running_Node node = {};
    node.stdio = stdio;
    node.local = allocator.alloc<Shell_Local>();
    *node.local = {};
    node.local->parent = parent_local;
    node.local->relationship = Shell_Local::ARGS_ONLY;
    return node;
}

static void expand_file_argument(cz::Str* path, Shell_Local* local, cz::Allocator allocator) {
    ZoneScoped;

    if (!path->starts_with("__tesh_std_")) {
        cz::String file = {};
        expand_arg_single(local, *path, allocator, &file);
        // Reallocate to ensure the file isn't null and also don't waste space.
        file.realloc_null_terminate(allocator);
        *path = file;
    }
}

static Error link_stdio(Stdio_State* stdio,
                        cz::Input_File* pipe_in,
                        const Parse_Program* parse_program,
                        cz::Allocator allocator,
                        cz::Slice<Parse_Node> program_nodes,
                        size_t p,
                        bool bind_stdin) {
    ZoneScoped;

    Stdio_State old_stdio = *stdio;

    // Bind stdin.
    if (parse_program && parse_program->in_file != "__tesh_std_in") {
        stdio->in.type = File_Type_File;
        stdio->in.file = {};
        stdio->in.count = nullptr;
    } else if (p > 0) {
        if (pipe_in->is_open()) {
            stdio->in.type = File_Type_Pipe;
            stdio->in.file = *pipe_in;
            stdio->in.count = allocator.alloc<size_t>();
            *stdio->in.count = 1;
        } else {
            stdio->in.type = File_Type_None;
            stdio->in.file = {};
            stdio->in.count = nullptr;
        }
    } else if (!bind_stdin) {
        stdio->in.type = File_Type_None;
        stdio->in.file = {};
        stdio->in.count = nullptr;
    } else {
        if (stdio->in.count)
            ++*stdio->in.count;
    }
    *pipe_in = {};

    // Bind stdout.
    if (parse_program && parse_program->out_file != "__tesh_std_out") {
        if (parse_program->out_file == "__tesh_std_err") {
            stdio->out = old_stdio.err;
            if (stdio->out.count)
                ++*stdio->out.count;
        } else {
            stdio->out.type = File_Type_File;
            stdio->out.file = {};
            stdio->out.count = nullptr;
        }
    } else if (p + 1 < program_nodes.len) {
        stdio->out.type = File_Type_Pipe;
    } else {
        if (stdio->out.count)
            ++*stdio->out.count;
    }

    // Bind stderr.
    if (parse_program && parse_program->err_file != "__tesh_std_err") {
        if (parse_program->err_file == "__tesh_std_out") {
            if (p + 1 < program_nodes.len) {
                stdio->err.type = File_Type_Pipe;
            } else {
                stdio->err = old_stdio.out;
                if (stdio->err.count)
                    ++*stdio->err.count;
            }
        } else {
            stdio->err.type = File_Type_File;
            stdio->err.file = {};
            stdio->err.count = nullptr;
        }
    } else {
        if (stdio->err.count)
            ++*stdio->err.count;
    }

    // Make pipes for the next iteration.
    if ((stdio->out.type == File_Type_Pipe || stdio->err.type == File_Type_Pipe) &&
        (p + 1 < program_nodes.len)) {
        bool create_pipe = true;

        // If next item in the pipeline doesn't read from stdin then this
        // program's stdout is dead so just leave it null and don't create a pipe.
        Parse_Node* next = &program_nodes[p + 1];
        if (next->type == Parse_Node::PROGRAM && next->v.program->in_file != "__tesh_std_in")
            create_pipe = false;

        if (create_pipe) {
            cz::Output_File pipe_out;
            if (!cz::create_pipe(pipe_in, &pipe_out))
                return Error_IO;

            if (!pipe_in->set_non_inheritable())
                return Error_IO;
            if (!pipe_out.set_non_inheritable())
                return Error_IO;

            size_t* count = allocator.alloc<size_t>();
            *count = 0;
            if (stdio->out.type == File_Type_Pipe) {
                stdio->out.file = pipe_out;
                stdio->out.count = count;
                ++*count;
            }
            if (stdio->err.type == File_Type_Pipe) {
                stdio->err.file = pipe_out;
                stdio->err.count = count;
                ++*count;
            }
        }
    }

    return Error_Success;
}

static void open_redirected_files(Stdio_State* stdio,
                                  cz::Str* error_path,
                                  const Parse_Program& parse_program,
                                  cz::Allocator allocator,
                                  Shell_Local* local) {
    cz::String path = {};
    if (stdio->in.type == File_Type_File && !parse_program.in_file.starts_with("__tesh_std_")) {
        if (parse_program.in_file == "/dev/null") {
            stdio->in.file = null_input;
            stdio->in.count = &null_input_count;
            ++*stdio->in.count;
        } else if (error_path->len == 0) {
            path.len = 0;
            cz::path::make_absolute(parse_program.in_file, get_wd(local), temp_allocator, &path);
            if (stdio->in.file.open(path.buffer)) {
                stdio->in.count = allocator.alloc<size_t>();
                *stdio->in.count = 1;
            } else {
                *error_path = parse_program.in_file;
            }
        }
    }

    if (stdio->out.type == File_Type_File && !parse_program.out_file.starts_with("__tesh_std_")) {
        if (parse_program.out_file == "/dev/null") {
            stdio->out.file = null_output;
            stdio->out.count = &null_output_count;
            ++*stdio->out.count;
        } else if (error_path->len == 0) {
            path.len = 0;
            cz::path::make_absolute(parse_program.out_file, get_wd(local), temp_allocator, &path);
            if (stdio->out.file.open(path.buffer)) {
                stdio->out.count = allocator.alloc<size_t>();
                *stdio->out.count = 1;
            } else {
                *error_path = parse_program.out_file;
            }
        }
    }

    if (stdio->err.type == File_Type_File && !parse_program.err_file.starts_with("__tesh_std_")) {
        if (parse_program.err_file == "/dev/null") {
            stdio->err.file = null_output;
            stdio->err.count = &null_output_count;
            ++*stdio->err.count;
        } else if (error_path->len == 0) {
            path.len = 0;
            cz::path::make_absolute(parse_program.err_file, get_wd(local), temp_allocator, &path);
            if (stdio->err.file.open(path.buffer)) {
                stdio->err.count = allocator.alloc<size_t>();
                *stdio->err.count = 1;
            } else {
                *error_path = parse_program.err_file;
            }
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
// Run program
///////////////////////////////////////////////////////////////////////////////

static void generate_environment(void* out,
                                 const Shell_Local* local,
                                 cz::Slice<const cz::Str> variable_names,
                                 cz::Slice<const cz::Str> variable_values);

static Error run_program(Shell_State* shell,
                         Shell_Local* local,
                         cz::Allocator allocator,
                         const Pseudo_Terminal& tty,
                         Running_Program* program,
                         Parse_Program parse,
                         Stdio_State stdio,
                         Backlog_State* backlog,
                         cz::Str error_path) {
    ZoneScoped;

    {
        cz::Vector<cz::Str> variable_values = {};
        variable_values.reserve_exact(allocator, parse.variable_values.len);
        for (size_t i = 0; i < parse.variable_values.len; ++i) {
            cz::String value = {};
            expand_arg_single(local, parse.variable_values[i], allocator, &value);
            variable_values.push(value);
        }
        parse.variable_values = variable_values;
    }

    // Parenthesized expression.  Fork (copy on write) vars.
    if (parse.is_sub) {
        program->type = Running_Program::SUB;
        program->v.sub = build_sub_running_node(local, stdio, allocator);

        program->v.sub.local->relationship = Shell_Local::COW;
        program->v.sub.local->exported_vars = local->exported_vars.clone(cz::heap_allocator());
        program->v.sub.local->variable_names = local->variable_names.clone(cz::heap_allocator());
        program->v.sub.local->variable_values = local->variable_values.clone(cz::heap_allocator());
        for (size_t i = 0; i < local->exported_vars.len; ++i) {
            local->exported_vars[i].increment();
        }
        for (size_t i = 0; i < local->variable_names.len; ++i) {
            local->variable_names[i].increment();
            local->variable_values[i].increment();
        }

        return start_execute_node(shell, tty, backlog, &program->v.sub, parse.v.sub);
    }

    // Expand arguments.
    cz::Vector<cz::Str> args = {};
    CZ_DEFER(args.drop(cz::heap_allocator()));
    for (size_t i = 0; i < parse.v.args.len; ++i) {
        expand_arg_split(local, parse.v.args[i], allocator, &args);
    }

    if (parse.v.args.len > 0 || args.len > 0) {
        // Lookup aliases based on the raw arguments, functions based on the expanded arguments.
        cz::Str alias_key = (parse.v.args.len > 0 ? parse.v.args[0] : "");
        cz::Str function_key = (args.len > 0 ? args[0] : "");
        Parse_Node* body;
        int result = get_alias_or_function(local, alias_key, function_key, &body);

        if (result != 0) {
            program->type = Running_Program::SUB;
            program->v.sub = build_sub_running_node(local, stdio, allocator);
            program->v.sub.local->args = args.clone(allocator);

            // Track the alias stack to prevent infinite recursion on 'alias ls=ls; ls'.
            if (result == 1) {
                program->v.sub.local->blocked_alias = parse.v.args[0];
            }

            return start_execute_node(shell, tty, backlog, &program->v.sub, body);
        }
    }

    parse.v.args = args;
    if (error_path.len > 0) {
        program->type = Running_Program::ANY_BUILTIN;
        program->v.builtin.command = Builtin_Command::INVALID;
        program->v.builtin.st.invalid = {};
        program->v.builtin.st.invalid.m1 = "cannot open file";
        program->v.builtin.st.invalid.m2 = error_path;
    } else {
        // Setup default as PROCESS then try to recognize ANY_BUILTIN.
        program->type = Running_Program::PROCESS;
        recognize_builtin(program, parse);
    }

    // If command is a builtin.
    if (program->type == Running_Program::ANY_BUILTIN) {
    make_builtin:
        setup_builtin(&program->v.builtin, allocator, stdio);

        if (stdio.in.type == File_Type_Pipe && !stdio.in.file.set_non_blocking())
            return Error_IO;
        if (stdio.out.type == File_Type_Pipe && !stdio.out.file.set_non_blocking())
            return Error_IO;
        if (stdio.err.type == File_Type_Pipe && !stdio.err.file.set_non_blocking())
            return Error_IO;

        if (stdio.in.type == File_Type_Terminal) {
            program->v.builtin.in.polling = true;
#ifdef _WIN32
            program->v.builtin.in.file = tty.child_in;
#else
            program->v.builtin.in.file.handle = tty.child_bi;
#endif
        } else {
            program->v.builtin.in.polling = false;
            program->v.builtin.in.file = stdio.in.file;
            program->v.builtin.in_count = stdio.in.count;
        }
        if (stdio.out.type == File_Type_Terminal) {
            program->v.builtin.out.type = Process_Output::BACKLOG;
            program->v.builtin.out.v.backlog = backlog;
        } else {
            program->v.builtin.out.type = Process_Output::FILE;
            program->v.builtin.out.v.file = stdio.out.file;
            program->v.builtin.out_count = stdio.out.count;
        }
        if (stdio.err.type == File_Type_Terminal) {
            program->v.builtin.err.type = Process_Output::BACKLOG;
            program->v.builtin.err.v.backlog = backlog;
        } else {
            program->v.builtin.err.type = Process_Output::FILE;
            program->v.builtin.err.v.file = stdio.err.file;
            program->v.builtin.err_count = stdio.err.count;
        }

        program->v.builtin.args = args.clone(allocator);
        program->v.builtin.working_directory = get_wd(local).clone_null_terminate(allocator);
        return Error_Success;
    }

    cz::String full_path = {};
    if (!find_in_path(local, args[0], allocator, &full_path)) {
        program->type = Running_Program::ANY_BUILTIN;
        program->v.builtin.command = Builtin_Command::INVALID;
        program->v.builtin.st.invalid = {};
        program->v.builtin.st.invalid.m1 = "cannot find in path";
        program->v.builtin.st.invalid.m2 = args[0];
        goto make_builtin;
    }
    args[0] = full_path;

#ifdef _WIN32
    if (args[0].ends_with_case_insensitive(".ps1")) {
        args.reserve(cz::heap_allocator(), 1);
        args.insert(0, "powershell");
    }
#endif

    // If spawning an actual program, we need to open the null file instead of passing a null fd.
    if (stdio.in.type == File_Type_None) {
        stdio.in.type = File_Type_File;
        stdio.in.file = null_input;
        stdio.in.count = &null_input_count;
        ++*stdio.in.count;
    }
    if (stdio.out.type == File_Type_None) {
        stdio.out.type = File_Type_File;
        stdio.out.file = null_output;
        stdio.out.count = &null_output_count;
        ++*stdio.out.count;
    }
    if (stdio.err.type == File_Type_None) {
        stdio.err.type = File_Type_File;
        stdio.err.file = null_output;
        stdio.err.count = &null_output_count;
        ++*stdio.err.count;
    }

    cz::Process_Options options;
#ifdef _WIN32
    if (stdio.in.type == File_Type_Terminal && stdio.out.type == File_Type_Terminal &&
        stdio.err.type == File_Type_Terminal) {
        // TODO: test pseudo console + stdio.
        options.pseudo_console = tty.pseudo_console;
    } else {
        if (stdio.in.type == File_Type_Terminal) {
            options.std_in = tty.child_in;
        } else {
            options.std_in = stdio.in.file;
        }
        if (stdio.out.type == File_Type_Terminal) {
            options.std_out = tty.child_out;
        } else {
            options.std_out = stdio.out.file;
        }
        if (stdio.err.type == File_Type_Terminal) {
            options.std_err = tty.child_out;  // yes, out!
        } else {
            options.std_err = stdio.err.file;
        }
    }
#else
    if (stdio.in.type == File_Type_Terminal) {
        options.std_in.handle = tty.child_bi;
    } else {
        options.std_in = stdio.in.file;
    }
    if (stdio.out.type == File_Type_Terminal) {
        options.std_out.handle = tty.child_bi;
    } else {
        options.std_out = stdio.out.file;
    }
    if (stdio.err.type == File_Type_Terminal) {
        options.std_err.handle = tty.child_bi;
    } else {
        options.std_err = stdio.err.file;
    }
#endif

    if (options.std_in.is_open() && !options.std_in.set_inheritable())
        return Error_IO;
    if (options.std_out.is_open() && !options.std_out.set_inheritable())
        return Error_IO;
    if (options.std_err.is_open() && !options.std_err.set_inheritable())
        return Error_IO;

    options.working_directory = get_wd(local).buffer;
    generate_environment(&options.environment, local, parse.variable_names, parse.variable_values);

    program->v.process = {};
    bool result = program->v.process.launch_program(args, options);

    if (options.std_in.is_open() && !options.std_in.set_non_inheritable())
        return Error_IO;
    if (options.std_out.is_open() && !options.std_out.set_non_inheritable())
        return Error_IO;
    if (options.std_err.is_open() && !options.std_err.set_non_inheritable())
        return Error_IO;

    close_rc_file(stdio.in.count, stdio.in.file);
    close_rc_file(stdio.out.count, stdio.out.file);
    close_rc_file(stdio.err.count, stdio.err.file);

    if (!result)
        return Error_IO;
    return Error_Success;
}

///////////////////////////////////////////////////////////////////////////////
// Environment control
///////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32
static void push_environment(cz::String* table, cz::Str key, cz::Str value) {
    table->reserve(temp_allocator, key.len + value.len + 3);
    table->append(key);
    table->push('=');
    table->append(value);
    table->push('\0');
}
#else
static void push_environment(cz::Vector<char*>* table, cz::Str key, cz::Str value) {
    cz::String entry = {};
    entry.reserve_exact(temp_allocator, key.len + value.len + 2);
    entry.append(key);
    entry.push('=');
    entry.append(value);
    entry.null_terminate();

    table->reserve(cz::heap_allocator(), 1);
    table->push(entry.buffer);
}
#endif

static void generate_environment(void* out_arg,
                                 const Shell_Local* local,
                                 cz::Slice<const cz::Str> variable_names,
                                 cz::Slice<const cz::Str> variable_values) {
#ifdef _WIN32
    cz::String table = {};
#else
    cz::Vector<char*> table = {};
    CZ_DEFER(table.drop(cz::heap_allocator()));
#endif

    for (size_t i = 0; i < variable_names.len; ++i) {
        cz::Str key = variable_names[i];
        for (size_t j = 0; j < i; ++j) {
            if (key == variable_names[j])
                goto skip1;
        }
        push_environment(&table, key, variable_values[i]);
    skip1:;
    }

    for (; local; local = local->parent) {
        if (local->relationship == Shell_Local::ARGS_ONLY)
            continue;

        for (size_t i = 0; i < local->exported_vars.len; ++i) {
            cz::Str key = local->exported_vars[i].str;
            for (size_t j = 0; j < variable_names.len; ++j) {
                if (key == variable_names[j])
                    goto skip2;
            }
            for (size_t j = 0; j < i; ++j) {
                if (key == local->exported_vars[j].str)
                    goto skip2;
            }
            cz::Str value;
            if (!get_var(local, key, &value))
                value = {};
            push_environment(&table, key, value);
        skip2:;
        }
    }

#ifdef _WIN32
    char** out = (char**)out_arg;
    table.reserve_exact(temp_allocator, 1);
    table.null_terminate();
    *out = table.buffer;
#else
    char*** out = (char***)out_arg;
    table.reserve(cz::heap_allocator(), 1);
    table.push(nullptr);
    *out = table.clone(temp_allocator).elems;
#endif
}

////////////////////////////////////////////////////////////////////////////////
// Print error
////////////////////////////////////////////////////////////////////////////////

static void print_error(Output_Object& out, Backlog_State* backlog, Error error) {
    Process_Output err = {};
    if (out.type == File_Type_Terminal) {
        err.type = Process_Output::BACKLOG;
        err.v.backlog = backlog;
    } else {
        err.type = Process_Output::FILE;
        err.v.file = out.file;
    }

    (void)err.write(cz::format(temp_allocator, "tesh: Error: ", error_string(error), "\n"));
}
