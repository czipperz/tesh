#pragma once

#include <stdint.h>
#include <chrono>
#include <cz/buffer_array.hpp>
#include <cz/process.hpp>
#include <cz/str.hpp>
#include <cz/vector.hpp>
#include "backlog.hpp"
#include "error.hpp"
#include "rcstr.hpp"
#include "render.hpp"

struct Running_Program;
struct Running_Pipeline;
struct Running_Script;
struct Parse_Line;
struct Pseudo_Terminal;
struct Shell_Node;
struct Prompt_State;

///////////////////////////////////////////////////////////////////////////////

struct Shell_Local {
    Shell_Local* parent;

    // For simplicity's sake, the array of variables is duplicated to subshells.  ('unset' builtin
    // causes too much complexity in variable lookup and environment generation code otherwise).
    cz::Vector<RcStr> exported_vars;
    cz::Vector<RcStr> variable_names;
    cz::Vector<RcStr> variable_values;

    cz::Vector<cz::String> alias_names;
    cz::Vector<Shell_Node*> alias_values;

    cz::Vector<cz::String> function_names;
    cz::Vector<Shell_Node*> function_values;

    cz::Vector<cz::Str> args;

    cz::Vector<cz::String> working_directories;

    cz::Str blocked_alias;

    enum {
        COW = 0,    // All writes are independent, reads are merged (except vars).
        ARGS_ONLY,  // Only arguments are independent.
    } relationship;
};

struct Shell_State {
    int width, height;

    Shell_Local local;

    cz::Vector<Running_Script> scripts;
    uint64_t selected_process = ~0ull;
    uint64_t attached_process = ~0ull;

    cz::Vector<cz::Buffer_Array> arenas;
};

bool get_var(const Shell_Local* local, cz::Str key, cz::Str* value);
void set_var(Shell_Local* local, cz::Str key, cz::Str value);
void unset_var(Shell_Local* local, cz::Str key);
void make_env_var(Shell_Local* local, cz::Str key);
cz::Str get_wd(const Shell_Local* local);
bool get_old_wd(const Shell_Local* local, size_t num, cz::Str* result);
void set_wd(Shell_Local* local, cz::Str value);
void set_alias(Shell_Local* local, cz::Str key, Shell_Node* node);
void set_function(Shell_Local* local, cz::Str key, Shell_Node* node);

/// Returns 0 on failure, 1 for alias, 2 for function.
int get_alias_or_function(const Shell_Local* local,
                          cz::Str alias_key,
                          cz::Str function_key,
                          Shell_Node** value);

void cleanup_processes(Shell_State* shell);
void recycle_process(Shell_State* shell, Running_Script* script);
void cleanup_pipeline(Running_Pipeline* script);
void recycle_pipeline(Shell_State* shell, Running_Pipeline* script);

void cleanup_local(Shell_Local* local);

cz::Buffer_Array alloc_arena(Shell_State* shell);
void recycle_arena(Shell_State* shell, cz::Buffer_Array arena);

void append_node(cz::Allocator allocator, cz::String* string, Shell_Node* node, bool add_semicolon);

/// Get the attached process, or `nullptr` if there is none.
Running_Script* attached_process(Shell_State* shell);
/// Get the selected process, or `nullptr` if there is none.
Running_Script* selected_process(Shell_State* shell);

/// Find a process by id, or `nullptr` if no matches.
Running_Script* lookup_process(Shell_State* shell, uint64_t id);

bool find_in_path(Shell_Local* local,
                  cz::Str abbreviation,
                  cz::Allocator allocator,
                  cz::String* full_path);

void cleanup_builtin(Running_Program* program);
void close_rc_file(size_t* count, cz::File_Descriptor file);

///////////////////////////////////////////////////////////////////////////////

struct Process_Output {
    enum {
        FILE,
        BACKLOG,
    } type;
    union {
        cz::Output_File file;
        Backlog_State* backlog;
    } v;

    int64_t write(cz::Str str) { return write(str.buffer, str.len); }
    int64_t write(const void* buffer, size_t len);
};

struct Process_Input {
    bool polling;
    bool done;
    cz::Input_File file;

    int64_t read(void* buffer, size_t len);
    int64_t read_text(char* buffer, size_t len, cz::Carriage_Return_Carry* carry);
};

///////////////////////////////////////////////////////////////////////////////

struct Pseudo_Terminal {
#ifdef _WIN32
    /// The child state.
    void* pseudo_console;
    cz::Input_File child_in;
    cz::Output_File child_out;
    /// The parent state.
    cz::Output_File in;
    cz::Input_File out;
#else
    /// The child state.
    int child_bi;
    /// The parent state.
    int parent_bi;
#endif
};

bool create_pseudo_terminal(Pseudo_Terminal* tty, int width, int height);
bool set_window_size(Pseudo_Terminal* tty, int width, int height);
void destroy_pseudo_terminal(Pseudo_Terminal* tty);
int64_t tty_write(Pseudo_Terminal* tty, cz::Str message);

///////////////////////////////////////////////////////////////////////////////

enum File_Type {
    File_Type_Terminal,
    File_Type_File,
    File_Type_Pipe,
    File_Type_None,
};

struct Stdio_State {
    File_Type in_type = File_Type_Terminal;
    File_Type out_type = File_Type_Terminal;
    File_Type err_type = File_Type_Terminal;
    cz::Input_File in;
    cz::Output_File out;
    cz::Output_File err;
    size_t* in_count;
    size_t* out_count;
    size_t* err_count;
};

void cleanup_stdio(Stdio_State* stdio);

struct Running_Node;
struct Running_Program;

struct Running_Pipeline {
    cz::Buffer_Array arena;
    cz::Vector<Shell_Node*> path;
    cz::Vector<Running_Program> programs;
    bool has_exit_code;
    int last_exit_code;
};

struct Running_Node {
    cz::Vector<Running_Pipeline> bg;
    Running_Pipeline fg;
    bool fg_finished;
    Stdio_State stdio;
    Shell_Local* local;
};

struct Running_Program {
    enum Type {
        PROCESS,
        SUB,
        ECHO,
        CAT,
        EXIT,
        RETURN,
        PWD,
        CD,
        LS,
        ALIAS,
        VARIABLES,
        WHICH,
        TRUE_,
        FALSE_,
        EXPORT,
        UNSET,
        CLEAR,
        SOURCE,
        SLEEP,
        CONFIGURE,
        ATTACH,
        FOLLOW,
        ARGDUMP,
        VARDUMP,
        SHIFT,
        HISTORY,
    } type;
    union {
        cz::Process process;
        Running_Node sub;
        struct {
            cz::Slice<const cz::Str> args;
            Process_Input in;
            Process_Output out;
            Process_Output err;
            size_t* in_count;
            size_t* out_count;
            size_t* err_count;
            cz::Str working_directory;  // null terminated
            int exit_code;
            union {
                struct {
                    size_t outer, inner;
                } echo;
                struct {
                    size_t outer;
                    Process_Input file;
                    cz::Carriage_Return_Carry carry;
                    char* buffer;
                    size_t len, offset;
                } cat;
                struct {
                    cz::Slice<const cz::Str> names;
                    cz::Slice<const cz::Str> values;
                } variables;
                struct {
                    std::chrono::high_resolution_clock::time_point start;
                } sleep;
                struct {
                    size_t outer, inner;
                } history;
                struct {
                    Stdio_State stdio;
                } source;
            } st;
        } builtin;
    } v;
};

struct Running_Script {
    uint64_t id;
    cz::Buffer_Array arena;
    Pseudo_Terminal tty;
    Running_Node root;
};

///////////////////////////////////////////////////////////////////////////////

struct Parse_Program {
    cz::Vector<cz::Str> variable_names;
    cz::Vector<cz::Str> variable_values;

    cz::Vector<Shell_Node*> subexprs;

    bool is_sub;
    union {
        cz::Vector<cz::Str> args;
        Shell_Node* sub;
    } v;

    /// Note: `Str::buffer == null` means not present.
    cz::Str in_file;
    cz::Str out_file;
    cz::Str err_file;
};

struct Shell_Node {
    enum Type {
        SEQUENCE,  // Put sequence first so memset(0) gets valid & empty node.
        PROGRAM,
        PIPELINE,
        AND,
        OR,
        IF,
        FUNCTION,
    };
    Type type : 7;
    uint8_t async : 1;
    union {
        cz::Vector<Shell_Node> sequence;
        Parse_Program* program;
        cz::Vector<Shell_Node> pipeline;
        struct {
            Shell_Node* left;
            Shell_Node* right;
        } binary;
        struct {
            Shell_Node* cond;
            Shell_Node* then;
            Shell_Node* other;  // NULL if no else statement.
        } if_;
        struct {
            cz::Str name;
            Shell_Node* body;
        } function;
    } v;
};

/// Parse a string into a `Shell_Node` tree.  Does not do variable expansion.
Error parse_script(cz::Allocator allocator, Shell_Node* root, cz::Str text);

void expand_arg_single(const Shell_Local* local,
                       cz::Str arg,
                       cz::Allocator allocator,
                       cz::String* output);
void expand_arg_split(const Shell_Local* local,
                      cz::Str arg,
                      cz::Allocator allocator,
                      cz::Vector<cz::Str>* output);

///////////////////////////////////////////////////////////////////////////////

Error start_execute_script(Shell_State* shell,
                           Backlog_State* backlog,
                           cz::Buffer_Array arena,
                           Shell_Node* root);

Error start_execute_node(Shell_State* shell,
                         const Pseudo_Terminal& tty,
                         Backlog_State* backlog,
                         Running_Node* node,
                         Shell_Node* root);

bool finish_line(Shell_State* shell,
                 const Pseudo_Terminal& tty,
                 Running_Node* node,
                 Backlog_State* backlog,
                 Running_Pipeline* line,
                 bool background);

///////////////////////////////////////////////////////////////////////////////

bool tick_running_node(Shell_State* shell,
                       cz::Slice<Backlog_State*> backlogs,
                       Render_State* rend,
                       Prompt_State* prompt,
                       Running_Node* node,
                       Pseudo_Terminal* tty,
                       Backlog_State* backlog,
                       bool* force_quit);
