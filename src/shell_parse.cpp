#include "shell.hpp"

#include <Tracy.hpp>
#include <cz/char_type.hpp>
#include <cz/defer.hpp>
#include <cz/heap.hpp>
#include <cz/sort.hpp>
#include <cz/string.hpp>

struct Parse_State {
    size_t in_file_i;
    size_t out_file_i;
    size_t err_file_i;
};

static Error parse_pipeline(const Shell_State* shell,
                            cz::Allocator allocator,
                            Parse_Pipeline* out,
                            cz::Str text,
                            size_t* index);

static Error finish_program(Parse_Pipeline* out,
                            cz::Allocator allocator,
                            Parse_Program* program,
                            Parse_State* state);
static Error handle_file_indirection(Parse_Program* program, Parse_State* state);
static bool get_var_at_point(cz::Str text, size_t index, cz::Str* key);

///////////////////////////////////////////////////////////////////////////////

Error parse_script(const Shell_State* shell,
                   cz::Allocator allocator,
                   Parse_Script* out,
                   cz::Str text) {
    size_t index = 0;

    Parse_Line* outer_success = nullptr;
    Parse_Line* outer_failure = nullptr;

    Parse_Line* line = &out->first;
    while (1) {
        Error error = parse_pipeline(shell, allocator, &line->pipeline, text, &index);
        if (error != Error_Success)
            return error;

        // End of script.
        if (index == text.len)
            return Error_Success;

        Parse_Line* next = allocator.alloc<Parse_Line>();
        CZ_ASSERT(next);
        *next = {};

        // Register continuation.
        switch (text[index]) {
        case '\n':
        case ';':
            line->on.success = next;
            line->on.failure = next;
            break;
        case '|':
            CZ_DEBUG_ASSERT(text[index + 1] == '|');
            ++index;
            line->on.success = outer_success;
            line->on.failure = next;
            break;
        case '&':
            if (text.slice_start(index + 1).starts_with('&')) {
                line->on.success = next;
                line->on.failure = outer_failure;
                ++index;
            } else {
                line->on.start = next;
            }
        }
        ++index;

        line = next;
    }
}

///////////////////////////////////////////////////////////////////////////////

static Error parse_pipeline(const Shell_State* shell,
                            cz::Allocator allocator,
                            Parse_Pipeline* out,
                            cz::Str text,
                            size_t* index) {
    ZoneScoped;

    Parse_Program program = {};
    Parse_State state = {};
    state.in_file_i = -1;
    state.out_file_i = -1;
    state.err_file_i = -1;

    CZ_DEFER({
        program.variable_names.drop(cz::heap_allocator());
        program.variable_values.drop(cz::heap_allocator());
        program.args.drop(cz::heap_allocator());
    });

    while (1) {
        // Ignore whitespace.
        while (*index < text.len && cz::is_blank(text[*index])) {
            ++*index;
        }

        // Done.
        if (*index == text.len) {
            break;
        }

        // Get the next word.
        size_t start_word = *index;
        cz::String key = {};
        cz::String word = {};
        bool allow_empty = false;
        bool any_special = false;
        bool special_stderr = false;
        while (*index < text.len) {
            switch (text[*index]) {
            case CZ_BLANK_CASES:
            case '|':
            case '&':
            case ';':
            case '\n':
                goto endofword;

            case '<':
            case '>':
                if (text.slice(start_word, *index) == "1") {
                    word.drop(allocator);
                    word = {};
                }
                if (text.slice(start_word, *index) == "2") {
                    special_stderr = true;
                    word.drop(allocator);
                    word = {};
                }
                goto endofword;

            case '\'': {
                allow_empty = true;
                any_special = true;
                ++*index;
                while (1) {
                    if (*index == text.len)
                        return Error_Parse;
                    char c = text[*index];
                    if (c == '\'')
                        break;
                    ++*index;
                    word.reserve(allocator, 1);
                    word.push(c);
                }
                break;
            }

            case '"': {
                allow_empty = true;
                any_special = true;
                ++*index;
                while (1) {
                    if (*index == text.len)
                        return Error_Parse;
                    char c = text[*index];
                    if (c == '"')
                        break;
                    ++*index;
                    if (c == '$') {
                        cz::Str key;
                        if (get_var_at_point(text, *index, &key)) {
                            *index += key.len;
                            cz::Str value;
                            if (get_var(shell, key, &value)) {
                                word.reserve(allocator, value.len);
                                word.append(value);
                            }
                            continue;
                        }
                    }
                    if (c == '\\') {
                        if (*index == text.len)
                            return Error_Parse;
                        char c2 = text[*index];
                        // From manual testing it looks like these are the only
                        // escape sequences that are processed.  Others are
                        // left (ie typing '\\' -> '\' but '\n' -> '\n').
                        if (c2 == '"' || c2 == '\\' || c2 == '`' || c2 == '$') {
                            c = c2;
                            ++*index;
                        }
                    }
                    word.reserve(allocator, 1);
                    word.push(c);
                }
                break;
            }

            case '=': {
                if (any_special || program.args.len > 0)
                    goto def;

                any_special = true;
                if (word.len == 0)
                    goto def;

                key = word;
                word = {};
                break;
            }

            case '$': {
                if (*index + 1 == text.len)
                    goto def;

                cz::Str var;
                if (!get_var_at_point(text, *index + 1, &var))
                    goto def;

                *index += var.len;

                cz::Str value;
                if (get_var(shell, var, &value)) {
                    if (key.len > 0) {
                        // a=$var is equivalent to a="$var"
                        word.reserve(allocator, value.len);
                        word.append(value);
                    } else {
                        // Split by whitespace.
                        size_t start = 0;
                        while (start < value.len) {
                            // Skip leading whitespace.
                            for (; start < value.len; ++start) {
                                if (!cz::is_space(value[start]))
                                    break;
                            }

                            size_t end = start;
                            for (; end < value.len; ++end) {
                                if (cz::is_space(value[end]))
                                    break;
                            }

                            word.reserve(allocator, end - start);
                            word.append(value.slice(start, end));

                            if (end < value.len) {
                                CZ_DEBUG_ASSERT(word.len > 0);
                                word.reserve_exact(allocator, 1);
                                word.null_terminate();
                                program.args.reserve(cz::heap_allocator(), 1);
                                program.args.push(word);
                                word = {};
                            }

                            start = end;
                        }
                    }
                }
                break;
            }

            case '\\':
                // Skip backslash newline.
                if (text.slice_start(*index + 1).starts_with('\n')) {
                    ++*index;
                    break;
                }
                goto def;

            default:
            def:
                word.reserve(allocator, 1);
                word.push(text[*index]);
                break;
            }
            ++*index;
        }

    endofword:
        if (key.len > 0) {
            program.variable_names.reserve(cz::heap_allocator(), 1);
            program.variable_values.reserve(cz::heap_allocator(), 1);
            program.variable_names.push(key);
            program.variable_values.push(word);
            continue;
        }

        // Push the word.
        if (allow_empty || word.len > 0) {
            word.reserve_exact(allocator, 1);
            word.null_terminate();
            program.args.reserve(cz::heap_allocator(), 1);
            program.args.push(word);
            continue;
        }

        // "echo $hi" when 'hi' is undefined should have 0 args.
        if (*index == text.len)
            break;

        // Special character.
        switch (text[*index]) {
        case '|': {
            if (text.slice_start(*index + 1).starts_with('|'))  // '||'
                goto break_outer;

            Error error = finish_program(out, allocator, &program, &state);
            if (error != Error_Success)
                return error;
            ++*index;
        } break;

        case '<':
        case '>': {
            bool is_stdin = text[*index] == '<';
            if (is_stdin) {
                state.in_file_i = program.args.len;
            } else if (special_stderr) {
                state.err_file_i = program.args.len;
            } else {
                state.out_file_i = program.args.len;
            }
            ++*index;
        } break;

        case ';':
        case '\n':
        case '&':
            goto break_outer;
        }
    }

break_outer:
    if (program.variable_names.len > 0 || program.args.len > 0) {
        return finish_program(out, allocator, &program, &state);
    }

    return Error_Success;
}

static Error finish_program(Parse_Pipeline* out,
                            cz::Allocator allocator,
                            Parse_Program* in,
                            Parse_State* state) {
    Parse_Program program = *in;
    program.variable_names = program.variable_names.clone(allocator);
    program.variable_values = program.variable_values.clone(allocator);
    program.args = program.args.clone(allocator);

    Error error = handle_file_indirection(&program, state);
    if (error != Error_Success)
        return error;

    out->pipeline.reserve(cz::heap_allocator(), 1);
    out->pipeline.push(program);

    in->variable_names.len = 0;
    in->variable_values.len = 0;
    in->args.len = 0;
    state->in_file_i = -1;
    state->out_file_i = -1;
    state->err_file_i = -1;
    return Error_Success;
}

static Error handle_file_indirection(Parse_Program* program, Parse_State* state) {
    size_t* indices[3];
    size_t num = 0;
    if (state->in_file_i != -1)
        indices[num++] = &state->in_file_i;
    if (state->out_file_i != -1)
        indices[num++] = &state->out_file_i;
    if (state->err_file_i != -1)
        indices[num++] = &state->err_file_i;
    cz::sort(cz::slice(indices, num),
             [](size_t** left, size_t** right) { return **left < **right; });
    for (size_t i = num; i-- > 0;) {
        size_t* index = indices[i];
        if (*index != -1) {
            if (*index >= program->args.len)
                return Error_Parse;
            if (index == &state->in_file_i)
                program->in_file = program->args[*index];
            else if (index == &state->out_file_i)
                program->out_file = program->args[*index];
            else
                program->err_file = program->args[*index];
            program->args.remove(*index);
        }
    }
    return Error_Success;
}

static bool get_var_at_point(cz::Str text, size_t index, cz::Str* key) {
    if (index == text.len)
        return false;

    size_t start = index;
    for (; index < text.len; ++index) {
        if (cz::is_alnum(text[index]))
            continue;
        break;
    }

    *key = text.slice(start, index);
    return true;
}
