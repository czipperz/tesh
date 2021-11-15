#pragma once

#include <stdint.h>
#include <chrono>
#include <cz/buffer_array.hpp>
#include <cz/process.hpp>
#include <cz/str.hpp>
#include <cz/vector.hpp>
#include "backlog.hpp"
#include "error.hpp"
#include "render.hpp"

struct Running_Pipeline;
struct Running_Script;
struct Parse_Line;
struct Pseudo_Terminal;

///////////////////////////////////////////////////////////////////////////////

struct Shell_State {
    int width, height;

    cz::Vector<cz::String> exported_vars;
    cz::Vector<cz::String> variable_names;
    cz::Vector<cz::String> variable_values;

    cz::Vector<cz::Str> alias_names;
    cz::Vector<cz::Str> alias_values;

    cz::Vector<Running_Script> scripts;
    uint64_t selected_process = ~0ull;
    uint64_t attached_process = ~0ull;

    cz::Vector<cz::Buffer_Array> arenas;

    cz::String working_directory;
};

bool get_var(const Shell_State* shell, cz::Str key, cz::Str* value);
void set_var(Shell_State* shell, cz::Str key, cz::Str value);
void make_env_var(Shell_State* shell, cz::Str key);
bool get_alias(const Shell_State* shell, cz::Str key, cz::Str* value);
void set_alias(Shell_State* shell, cz::Str key, cz::Str value);

void cleanup_processes(Shell_State* shell);
void recycle_process(Shell_State* shell, Running_Script* script);
void recycle_pipeline(Shell_State* shell, Running_Pipeline* script);

cz::Buffer_Array alloc_arena(Shell_State* shell);
void recycle_arena(Shell_State* shell, cz::Buffer_Array arena);

/// Get the attached process, or `nullptr` if there is none.
Running_Script* attached_process(Shell_State* shell);
/// Get the selected process, or `nullptr` if there is none.
Running_Script* selected_process(Shell_State* shell);

/// Find a process by id, or `nullptr` if no matches.
Running_Script* lookup_process(Shell_State* shell, uint64_t id);

bool find_in_path(Shell_State* shell,
                  cz::Str abbreviation,
                  cz::Allocator allocator,
                  cz::String* full_path);

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
    cz::Carriage_Return_Carry out_carry;
};

bool create_pseudo_terminal(Pseudo_Terminal* tty, int width, int height);
bool set_window_size(Pseudo_Terminal* tty, int width, int height);
void destroy_pseudo_terminal(Pseudo_Terminal* tty);

///////////////////////////////////////////////////////////////////////////////

struct Running_Program {
    enum Type {
        PROCESS,
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
        CLEAR,
        SOURCE,
        SLEEP,
    } type;
    union {
        cz::Process process;
        struct {
            cz::Slice<const cz::Str> args;
            cz::Input_File in;
            Process_Output out;
            Process_Output err;
            cz::Str working_directory;  // null terminated
            int exit_code;
            union {
                struct {
                    size_t outer, inner;
                } echo;
                struct {
                    size_t outer;
                    cz::Input_File file;
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
            } st;
        } builtin;
    } v;
};

struct Running_Pipeline {
    cz::Str command_line;
    cz::Vector<Running_Program> pipeline;
    size_t length;
    int last_exit_code;
    cz::Vector<cz::File_Descriptor> files;
    cz::Buffer_Array arena;
};

struct Parse_Continuation {
    Parse_Line* start;
    Parse_Line* success;
    Parse_Line* failure;
};

struct Running_Line {
    Running_Pipeline pipeline;
    Parse_Continuation on;
};

struct Running_Script {
    uint64_t id;
    cz::Buffer_Array arena;

    cz::Vector<Running_Line> bg;
    Running_Line fg;
    bool fg_finished;

    Pseudo_Terminal tty;
};

///////////////////////////////////////////////////////////////////////////////

struct Parse_Program {
    cz::Vector<cz::Str> variable_names;
    cz::Vector<cz::Str> variable_values;
    cz::Vector<cz::Str> args;

    /// Note: `Str::buffer == null` means not present.
    cz::Str in_file;
    cz::Str out_file;
    cz::Str err_file;
};

struct Parse_Pipeline {
    cz::Vector<Parse_Program> pipeline;
};

struct Parse_Line {
    Parse_Pipeline pipeline;
    Parse_Continuation on;
};

struct Parse_Script {
    Parse_Line first;
};

Error parse_script(const Shell_State* shell,
                   cz::Allocator allocator,
                   Parse_Script* out,
                   Parse_Continuation outer,
                   cz::Str text);

///////////////////////////////////////////////////////////////////////////////

Error start_execute_script(Shell_State* shell,
                           Backlog_State* backlog,
                           cz::Buffer_Array arena,
                           const Parse_Script& script,
                           cz::Str command_line);

Error start_execute_line(Shell_State* shell,
                         Backlog_State* backlog,
                         Running_Script* running_script,
                         const Parse_Line& line,
                         bool background);

///////////////////////////////////////////////////////////////////////////////

bool tick_program(Shell_State* shell,
                  Render_State* rend,
                  cz::Slice<Backlog_State*> backlogs,
                  Backlog_State* backlog,
                  Running_Script* script,
                  Running_Line* line,
                  Running_Program* program,
                  int* exit_code,
                  bool* force_exit);
