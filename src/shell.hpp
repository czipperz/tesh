#pragma once

#include <stdint.h>
#include <cz/process.hpp>
#include <cz/str.hpp>
#include <cz/vector.hpp>

///////////////////////////////////////////////////////////////////////////////

struct Running_Program {
    cz::Process process;
    uint64_t id;
};

struct Shell_State {
    // TODO: get a hashmap in here
    cz::Vector<cz::Str> variable_names;
    // TODO: make refcounted
    cz::Vector<cz::Str> variable_values;

    cz::Vector<Running_Program> programs;
};
