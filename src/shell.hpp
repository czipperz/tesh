#pragma once

#include <stdint.h>
#include <cz/buffer_array.hpp>
#include <cz/process.hpp>
#include <cz/str.hpp>
#include <cz/vector.hpp>
#include "backlog.hpp"
#include "error.hpp"

struct Running_Line;

///////////////////////////////////////////////////////////////////////////////

struct Shell_State {
    cz::Vector<cz::String> variable_names;
    cz::Vector<cz::String> variable_values;

    cz::Vector<cz::Str> alias_names;
    cz::Vector<cz::Str> alias_values;

    cz::Vector<Running_Line> lines;
    uint64_t active_process = ~0ull;

    cz::Vector<cz::Buffer_Array> arenas;

    cz::String working_directory;
};

bool get_env_var(const Shell_State* shell, cz::Str key, cz::Str* value);
void set_env_var(Shell_State* shell, cz::Str key, cz::Str value);

void cleanup_process(Running_Line* line);
void kill_process(Running_Line* line);
void cleanup_processes(Shell_State* shell);
void recycle_process(Shell_State* shell, Running_Line* line);

/// Get the active process, or `nullptr` if there is none.
Running_Line* active_process(Shell_State* shell);

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

struct Running_Line {
    uint64_t id;
    cz::Str command_line;
    cz::Vector<Running_Program> pipeline;
    cz::Vector<cz::File_Descriptor> files;
    cz::Output_File in;
    cz::Input_File out;
    cz::Carriage_Return_Carry out_carry;
    cz::Buffer_Array arena;
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

struct Parse_Line {
    cz::Vector<Parse_Program> pipeline;
};

Error parse_line(const Shell_State* shell, cz::Allocator allocator, Parse_Line* out, cz::Str text);

///////////////////////////////////////////////////////////////////////////////

Error start_execute_line(Shell_State* shell,
                         Backlog_State* backlog,
                         cz::Buffer_Array arena,
                         const Parse_Line& line,
                         cz::Str command_line,
                         uint64_t id);

///////////////////////////////////////////////////////////////////////////////

bool tick_program(Shell_State* shell, Running_Program* program, int* exit_code, bool* force_exit);
