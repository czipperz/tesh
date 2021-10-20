#pragma once

#include <cz/str.hpp>
#include <cz/vector.hpp>
#include "error.hpp"
#include "shell.hpp"

///////////////////////////////////////////////////////////////////////////////

struct Shell_Program {
    cz::Vector<cz::Str> args;
};

struct Shell_Line {
    cz::Vector<Shell_Program> pipeline;
};

///////////////////////////////////////////////////////////////////////////////

Error parse_line(const Shell_State* shell, cz::Allocator allocator, Shell_Line* out, cz::Str text);
