#pragma once

#include <stdint.h>
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
    } type;
    union {
        cz::Process process;
        struct {
            cz::Slice<const cz::Str> args;
            cz::Input_File in;
            cz::Output_File out;
            cz::Output_File err;
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
};

struct Shell_State {
    // TODO: get a hashmap in here
    cz::Vector<cz::Str> variable_names;
    // TODO: make refcounted
    cz::Vector<cz::Str> variable_values;

    cz::Vector<Running_Line> lines;
};

///////////////////////////////////////////////////////////////////////////////

struct Parse_Program {
    cz::Vector<cz::Str> args;
};

struct Parse_Line {
    cz::Vector<Parse_Program> pipeline;
};

///////////////////////////////////////////////////////////////////////////////

Error parse_line(const Shell_State* shell, cz::Allocator allocator, Parse_Line* out, cz::Str text);

///////////////////////////////////////////////////////////////////////////////

Error start_execute_line(Shell_State* shell, const Parse_Line& line, uint64_t id);

///////////////////////////////////////////////////////////////////////////////

bool tick_program(Running_Program* program, int* exit_code);
