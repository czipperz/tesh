#pragma once

#include <stdint.h>
#include <cz/buffer_array.hpp>
#include <cz/process.hpp>
#include <cz/str.hpp>
#include <cz/vector.hpp>
#include "error.hpp"

///////////////////////////////////////////////////////////////////////////////

struct Running_Program {
    enum Type {
        PROCESS,
        ECHO,
        CAT,
        EXIT,
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
            cz::Output_File out;
            cz::Output_File err;
            cz::Str working_directory;  // null terminated
            bool close_err;
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
    cz::Vector<Running_Program> pipeline;
    cz::Output_File in;
    cz::Input_File out;
    cz::Carriage_Return_Carry out_carry;
    cz::Buffer_Array arena;
};

struct Parse_Line;

struct Shell_State {
    cz::Vector<cz::String> variable_names;
    cz::Vector<cz::String> variable_values;

    cz::Vector<cz::Str> alias_names;
    cz::Vector<cz::Str> alias_values;

    cz::Vector<Running_Line> lines;

    cz::Vector<cz::Buffer_Array> arenas;

    cz::String working_directory;
};

bool get_env_var(const Shell_State* shell, cz::Str key, cz::Str* value);
void set_env_var(Shell_State* shell, cz::Str key, cz::Str value);

///////////////////////////////////////////////////////////////////////////////

struct Parse_Program {
    cz::Vector<cz::Str> variable_names;
    cz::Vector<cz::Str> variable_values;
    cz::Vector<cz::Str> args;
};

struct Parse_Line {
    cz::Vector<Parse_Program> pipeline;
};

Error parse_line(const Shell_State* shell, cz::Allocator allocator, Parse_Line* out, cz::Str text);

///////////////////////////////////////////////////////////////////////////////

Error start_execute_line(Shell_State* shell,
                         cz::Buffer_Array arena,
                         const Parse_Line& line,
                         uint64_t id);

///////////////////////////////////////////////////////////////////////////////

bool tick_program(Shell_State* shell, Running_Program* program, int* exit_code);
