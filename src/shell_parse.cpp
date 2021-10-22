#include "shell.hpp"

#include <Tracy.hpp>
#include <cz/char_type.hpp>
#include <cz/defer.hpp>
#include <cz/heap.hpp>
#include <cz/string.hpp>

static bool get_var_at_point(cz::Str text, size_t index, cz::Str* key);

Error parse_line(const Shell_State* shell, cz::Allocator allocator, Parse_Line* out, cz::Str text) {
    ZoneScoped;

    cz::Vector<cz::Str> variable_names = {};
    CZ_DEFER(variable_names.drop(cz::heap_allocator()));
    cz::Vector<cz::Str> variable_values = {};
    CZ_DEFER(variable_values.drop(cz::heap_allocator()));

    cz::Vector<cz::Str> args = {};
    CZ_DEFER(args.drop(cz::heap_allocator()));

    size_t index = 0;

    while (1) {
        // Ignore whitespace.
        while (index < text.len && cz::is_blank(text[index])) {
            ++index;
        }

        // Done.
        if (index == text.len) {
            break;
        }

        // Get the next word.
        cz::String key = {};
        cz::String word = {};
        bool allow_empty = false;
        bool any_special = false;
        while (index < text.len) {
            switch (text[index]) {
            case CZ_BLANK_CASES:
            case '|':
                goto endofword;

            case '\'': {
                allow_empty = true;
                any_special = true;
                ++index;
                while (1) {
                    if (index == text.len)
                        return Error_Parse;
                    char c = text[index];
                    if (c == '\'')
                        break;
                    ++index;
                    word.reserve(allocator, 1);
                    word.push(c);
                }
                break;
            }

            case '"': {
                allow_empty = true;
                any_special = true;
                ++index;
                while (1) {
                    if (index == text.len)
                        return Error_Parse;
                    char c = text[index];
                    if (c == '"')
                        break;
                    ++index;
                    if (c == '$') {
                        cz::Str key;
                        if (get_var_at_point(text, index, &key)) {
                            index += key.len;
                            cz::Str value;
                            if (get_env_var(shell, key, &value)) {
                                word.reserve(allocator, value.len);
                                word.append(value);
                            }
                            continue;
                        }
                    }
                    if (c == '\\') {
                        if (index == text.len)
                            return Error_Parse;
                        char c2 = text[index];
                        // From manual testing it looks like these are the only
                        // escape sequences that are processed.  Others are
                        // left (ie typing '\\' -> '\' but '\n' -> '\n').
                        if (c2 == '"' || c2 == '\\' || c2 == '`' || c2 == '$') {
                            c = c2;
                            ++index;
                        }
                    }
                    word.reserve(allocator, 1);
                    word.push(c);
                }
                break;
            }

            case '=': {
                if (any_special || args.len > 0)
                    goto def;

                any_special = true;
                if (word.len == 0)
                    goto def;

                key = word;
                word = {};
                break;
            }

            case '$': {
                if (index + 1 == text.len)
                    goto def;

                cz::Str var;
                if (!get_var_at_point(text, index + 1, &var))
                    goto def;

                index += var.len;

                cz::Str value;
                if (get_env_var(shell, var, &value)) {
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
                                args.reserve(cz::heap_allocator(), 1);
                                args.push(word);
                                word = {};
                            }

                            start = end;
                        }
                    }
                }
                break;
            }

            default:
            def:
                word.reserve(allocator, 1);
                word.push(text[index]);
                break;
            }
            ++index;
        }

    endofword:
        if (key.len > 0) {
            variable_names.reserve(cz::heap_allocator(), 1);
            variable_values.reserve(cz::heap_allocator(), 1);
            variable_names.push(key);
            variable_values.push(word);
            continue;
        }

        // Push the word.
        if (allow_empty || word.len > 0) {
            args.reserve(cz::heap_allocator(), 1);
            args.push(word);
            continue;
        }

        // "echo $hi" when 'hi' is undefined should have 0 args.
        if (index == text.len)
            break;

        // Special character.
        switch (text[index]) {
        case '|': {
            out->pipeline.reserve(cz::heap_allocator(), 1);
            Parse_Program program = {};
            program.args = args.clone(allocator);
            out->pipeline.push(program);
            args.len = 0;
            ++index;
        } break;
        }
    }

    if (variable_names.len > 0 || args.len > 0) {
        out->pipeline.reserve(cz::heap_allocator(), 1);
        Parse_Program program = {};
        program.variable_names = variable_names.clone(allocator);
        program.variable_values = variable_values.clone(allocator);
        program.args = args.clone(allocator);
        out->pipeline.push(program);
        args.len = 0;
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
