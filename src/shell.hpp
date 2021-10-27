#pragma once

#include <stdint.h>
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

///////////////////////////////////////////////////////////////////////////////

struct Shell_State {
    cz::Vector<cz::String> exported_vars;
    cz::Vector<cz::String> variable_names;
    cz::Vector<cz::String> variable_values;

    cz::Vector<cz::Str> alias_names;
    cz::Vector<cz::Str> alias_values;

    cz::Vector<Running_Script> scripts;
    uint64_t active_process = ~0ull;

    cz::Vector<cz::Buffer_Array> arenas;

    cz::String working_directory;
};

bool get_var(const Shell_State* shell, cz::Str key, cz::Str* value);
void set_var(Shell_State* shell, cz::Str key, cz::Str value);
void make_env_var(Shell_State* shell, cz::Str key);

void cleanup_processes(Shell_State* shell);
void recycle_process(Shell_State* shell, Running_Script* script);
void recycle_pipeline(Shell_State* shell, Running_Pipeline* script);

cz::Buffer_Array alloc_arena(Shell_State* shell);
void recycle_arena(Shell_State* shell, cz::Buffer_Array arena);

/// Get the active process, or `nullptr` if there is none.
Running_Script* active_process(Shell_State* shell);

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
        struct {
            Backlog_State* state;
            uint64_t process_id;
        } backlog;
    } v;

    int64_t write(cz::Str str) { return write(str.buffer, str.len); }
    int64_t write(const void* buffer, size_t len);
};

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
                    char* buffer;
                    size_t len, offset;
                } cat;
                struct {
                    cz::Slice<const cz::Str> names;
                    cz::Slice<const cz::Str> values;
                } variables;
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
    cz::Output_File in;
    cz::Input_File out;
    cz::Carriage_Return_Carry out_carry;

    /// Default stdin/stdout for a newly launched program in this script.
    cz::Input_File script_in;
    cz::Output_File script_out;
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
                           cz::Str command_line,
                           uint64_t id);

Error start_execute_line(Shell_State* shell,
                         Backlog_State* backlog,
                         Running_Script* running_script,
                         const Parse_Line& line,
                         bool background);

///////////////////////////////////////////////////////////////////////////////

bool tick_program(Shell_State* shell,
                  Render_State* rend,
                  Backlog_State* backlog,
                  Running_Program* program,
                  int* exit_code,
                  bool* force_exit);
