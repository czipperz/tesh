#pragma once

#include <cz/str.hpp>
#include <cz/vector.hpp>
#include "error.hpp"
#include "shell.hpp"

///////////////////////////////////////////////////////////////////////////////

struct Parse_Program {
    cz::Vector<cz::Str> args;
};

struct Parse_Line {
    cz::Vector<Parse_Program> pipeline;
};

///////////////////////////////////////////////////////////////////////////////

Error parse_line(const Shell_State* shell, cz::Allocator allocator, Parse_Line* out, cz::Str text);
