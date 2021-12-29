#pragma once

#include <cz/buffer_array.hpp>
#include <cz/string.hpp>
#include <cz/vector.hpp>

// Prompt_Edit::type bitfield values:
#define PROMPT_MOVE_INDEP 0x0
#define PROMPT_MOVE_BEFORE 0x1
#define PROMPT_MOVE_AFTER 0x2
#define PROMPT_EDIT_INSERT 0x0
#define PROMPT_EDIT_REMOVE 0x4
#define PROMPT_EDIT_MERGE 0x8
#define PROMPT_COMBO_START 0x10
#define PROMPT_COMBO_END 0x20

struct Prompt_Edit {
    uint32_t type;
    size_t position;
    cz::Str value;
};

struct Prompt_State {
    cz::Str prefix;

    cz::String text;
    size_t cursor;
    cz::Vector<Prompt_Edit> edit_history;
    size_t edit_index;
    cz::Buffer_Array edit_arena;

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

// Walk the undo tree.
void clear_undo_tree(Prompt_State* prompt);
bool undo(Prompt_State* prompt);
bool redo(Prompt_State* prompt);

// Combine multiple edits into one undo node.
void start_combo(Prompt_State* prompt);
void end_combo(Prompt_State* prompt);

// Push an edit.
void insert(Prompt_State* prompt, size_t index, cz::Str text);
void remove(Prompt_State* prompt, size_t start, size_t end);

// Push an edit and move the cursor.
void insert_before(Prompt_State* prompt, size_t index, cz::Str text);
void insert_after(Prompt_State* prompt, size_t index, cz::Str text);
void remove_before(Prompt_State* prompt, size_t start, size_t end);
void remove_after(Prompt_State* prompt, size_t start, size_t end);
