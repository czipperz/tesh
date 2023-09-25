#include "prompt.hpp"

#include <cz/heap.hpp>

////////////////////////////////////////////////////////////////////////////////
// Lifecycle
////////////////////////////////////////////////////////////////////////////////

void Prompt_State::init() {
    edit_arena.init();
    history_arena.init();
    completion.results_arena.init();
}

void Prompt_State::drop() {
    edit_arena.drop();
    history_arena.drop();
    completion.results_arena.drop();
}

///////////////////////////////////////////////////////////////////////////////
// Walk the undo tree
///////////////////////////////////////////////////////////////////////////////

void clear_undo_tree(Prompt_State* prompt) {
    prompt->edit_history.len = 0;
    prompt->edit_index = 0;
    prompt->edit_arena.clear();
}

bool undo(Prompt_State* prompt) {
    if (prompt->edit_index == 0)
        return false;

    size_t depth = 0;
    while (1) {
        // Remember: everything here is reversed because we are going backwards!
        Prompt_Edit* edit = &prompt->edit_history[--prompt->edit_index];
        if (edit->type & PROMPT_COMBO_START) {
            CZ_DEBUG_ASSERT(depth > 0);
            --depth;
        } else if (edit->type & PROMPT_COMBO_END) {
            ++depth;
        } else if (edit->type & PROMPT_EDIT_REMOVE) {
            // Undo remove = actually insert.
            prompt->text.insert(edit->position, edit->value);
            if (edit->type & PROMPT_MOVE_BEFORE)
                prompt->cursor = edit->position + edit->value.len;
            else if (edit->type & PROMPT_MOVE_AFTER)
                prompt->cursor = edit->position;
        } else {
            // Undo insert = actually remove.
            prompt->text.remove_many(edit->position, edit->value.len);
            if (edit->type & PROMPT_MOVE_BEFORE)
                prompt->cursor = edit->position;
            else if (edit->type & PROMPT_MOVE_AFTER)
                prompt->cursor = edit->position;
        }

        if (depth == 0)
            break;
    }

    return true;
}

bool redo(Prompt_State* prompt) {
    if (prompt->edit_index == prompt->edit_history.len)
        return false;

    size_t depth = 0;
    while (1) {
        Prompt_Edit* edit = &prompt->edit_history[prompt->edit_index++];
        if (edit->type & PROMPT_COMBO_START) {
            ++depth;
        } else if (edit->type & PROMPT_COMBO_END) {
            CZ_DEBUG_ASSERT(depth > 0);
            --depth;
        } else if (edit->type & PROMPT_EDIT_REMOVE) {
            // Redo remove = actually remove.
            prompt->text.remove_many(edit->position, edit->value.len);
            if (edit->type & PROMPT_MOVE_BEFORE)
                prompt->cursor = edit->position;
            else if (edit->type & PROMPT_MOVE_AFTER)
                prompt->cursor = edit->position;
        } else {
            // Redo insert = actually insert.
            prompt->text.insert(edit->position, edit->value);
            if (edit->type & PROMPT_MOVE_BEFORE)
                prompt->cursor = edit->position + edit->value.len;
            else if (edit->type & PROMPT_MOVE_AFTER)
                prompt->cursor = edit->position;
        }

        if (depth == 0)
            break;
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// Utility
///////////////////////////////////////////////////////////////////////////////

static void push_edit(Prompt_State* prompt, const Prompt_Edit& edit) {
    prompt->edit_history.len = prompt->edit_index;
    prompt->edit_history.reserve(cz::heap_allocator(), 1);
    prompt->edit_history.push(edit);
    prompt->edit_index++;
}

///////////////////////////////////////////////////////////////////////////////
// Combine multiple edits into one undo node.
///////////////////////////////////////////////////////////////////////////////

void start_combo(Prompt_State* prompt) {
    Prompt_Edit edit = {};
    edit.type = PROMPT_COMBO_START;
    push_edit(prompt, edit);
}

void end_combo(Prompt_State* prompt) {
    Prompt_Edit edit = {};
    edit.type = PROMPT_COMBO_END;
    push_edit(prompt, edit);
}

///////////////////////////////////////////////////////////////////////////////
// Push an edit.
///////////////////////////////////////////////////////////////////////////////

void insert(Prompt_State* prompt, size_t index, cz::Str text) {
    Prompt_Edit edit = {};
    edit.type = PROMPT_MOVE_INDEP | PROMPT_EDIT_INSERT;
    edit.position = index;
    edit.value = text.clone(prompt->edit_arena.allocator());
    push_edit(prompt, edit);

    prompt->text.reserve(cz::heap_allocator(), text.len);
    prompt->text.insert(index, text);
}

void insert_before(Prompt_State* prompt, size_t index, cz::Str text) {
    Prompt_Edit edit = {};
    edit.type = PROMPT_MOVE_BEFORE | PROMPT_EDIT_INSERT;
    edit.position = index;
    edit.value = text.clone(prompt->edit_arena.allocator());
    push_edit(prompt, edit);

    prompt->text.reserve(cz::heap_allocator(), text.len);
    prompt->text.insert(index, text);
    prompt->cursor = index + text.len;
}

void insert_after(Prompt_State* prompt, size_t index, cz::Str text) {
    Prompt_Edit edit = {};
    edit.type = PROMPT_MOVE_AFTER | PROMPT_EDIT_INSERT;
    edit.position = index;
    edit.value = text.clone(prompt->edit_arena.allocator());
    push_edit(prompt, edit);

    prompt->text.reserve(cz::heap_allocator(), text.len);
    prompt->text.insert(index, text);
    prompt->cursor = index;
}

void remove(Prompt_State* prompt, size_t start, size_t end) {
    Prompt_Edit edit = {};
    edit.type = PROMPT_MOVE_INDEP | PROMPT_EDIT_REMOVE;
    edit.position = start;
    edit.value = prompt->text.slice(start, end).clone(prompt->edit_arena.allocator());
    push_edit(prompt, edit);

    prompt->text.remove_range(start, end);
}

void remove_before(Prompt_State* prompt, size_t start, size_t end) {
    Prompt_Edit edit = {};
    edit.type = PROMPT_MOVE_BEFORE | PROMPT_EDIT_REMOVE;
    edit.position = start;
    edit.value = prompt->text.slice(start, end).clone(prompt->edit_arena.allocator());
    push_edit(prompt, edit);

    prompt->text.remove_range(start, end);
    prompt->cursor = start;
}

void remove_after(Prompt_State* prompt, size_t start, size_t end) {
    Prompt_Edit edit = {};
    edit.type = PROMPT_MOVE_AFTER | PROMPT_EDIT_REMOVE;
    edit.position = start;
    edit.value = prompt->text.slice(start, end).clone(prompt->edit_arena.allocator());
    push_edit(prompt, edit);

    prompt->text.remove_range(start, end);
    prompt->cursor = start;
}

cz::Vector<cz::Str>* prompt_history(Prompt_State* prompt, bool script) {
    return script ? &prompt->stdin_history : &prompt->history;
}
