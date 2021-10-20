#pragma once

#include <cz/vector.hpp>

///////////////////////////////////////////////////////////////////////////////

struct Shell_State {
    // TODO: get a hashmap in here
    cz::Vector<cz::Str> variable_names;
    // TODO: make refcounted
    cz::Vector<cz::Str> variable_values;
};
