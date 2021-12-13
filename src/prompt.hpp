#pragma once

#include <cz/buffer_array.hpp>
#include <cz/str.hpp>
#include <cz/vector.hpp>

struct Prompt_State {
    cz::Str prefix;
    cz::String text;
    size_t cursor;
    uint64_t process_id;
    uint64_t history_counter;
    cz::Vector<cz::Str> history;
    cz::Vector<cz::Str> stdin_history;
    cz::Buffer_Array history_arena;
    bool history_searching;

    struct {
        bool is;
        size_t prefix_length;
        cz::Buffer_Array results_arena;
        cz::Vector<cz::Str> results;
        size_t current;
    } completion;
};
