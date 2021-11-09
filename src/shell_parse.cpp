#include "shell.hpp"

#include <Tracy.hpp>
#include <cz/char_type.hpp>
#include <cz/defer.hpp>
#include <cz/heap.hpp>
#include <cz/sort.hpp>
#include <cz/string.hpp>

struct Text_Item {
    cz::Str text;
    size_t index;
    cz::String alias_key;
};

struct Text_Iter {
    cz::Vector<Text_Item> stack;

    void drop() {
        for (size_t i = 1; i < stack.len; ++i) {
            stack[i].alias_key.drop(cz::heap_allocator());
        }
        stack.drop(cz::heap_allocator());
    }

    void push(cz::Str text, cz::String alias_key) {
        stack.reserve(cz::heap_allocator(), 1);
        stack.push({text, 0, alias_key});
    }

    bool at_eob() const { return stack.len == 0; }

    char get() const {
        const Text_Item* item = &stack.last();
        if (item->index == item->text.len)
            return ' ';
        return item->text[item->index];
    }

    char peek(size_t off) const {
        const Text_Item* item = &stack.last();
        if (item->index + off >= item->text.len)
            return ' ';
        return item->text[item->index + off];
    }

    char before(size_t off) const {
        const Text_Item* item = &stack.last();
        if (item->index < off)
            return ' ';
        return item->text[item->index - off];
    }

    void advance() {
        Text_Item* item = &stack.last();
        size_t pad = (stack.len > 1 ? 1 : 0);
        if (item->index + 1 < item->text.len + pad) {
            ++item->index;
            return;
        }

        item->alias_key.drop(cz::heap_allocator());
        stack.pop();
    }

    void retreat() {
        Text_Item* item = &stack.last();
        CZ_DEBUG_ASSERT(item->index > 0);
        --item->index;
    }
};

struct Parse_State {
    size_t in_file_i;
    size_t out_file_i;
    size_t err_file_i;
};

static Error parse_pipeline(const Shell_State* shell,
                            cz::Allocator allocator,
                            Parse_Pipeline* out,
                            Text_Iter* it);

static Error finish_program(Parse_Pipeline* out,
                            cz::Allocator allocator,
                            Parse_Program* program,
                            Parse_State* state);
static Error handle_file_indirection(Parse_Program* program, Parse_State* state);
static bool get_var_at_point(Text_Iter* it, cz::Str* key);

///////////////////////////////////////////////////////////////////////////////

Error parse_script(const Shell_State* shell,
                   cz::Allocator allocator,
                   Parse_Script* out,
                   Parse_Continuation outer,
                   cz::Str text) {
    Text_Iter it = {};
    CZ_DEFER(it.drop());

    it.push(text, {});

    Parse_Line* line = &out->first;
    while (1) {
        Error error = parse_pipeline(shell, allocator, &line->pipeline, &it);
        if (error != Error_Success)
            return error;

        // End of script.
        if (it.at_eob()) {
            line->on = outer;
            return Error_Success;
        }

        Parse_Line* next = allocator.alloc<Parse_Line>();
        CZ_ASSERT(next);
        *next = {};

        // Register continuation.
        switch (it.get()) {
        case '\n':
        case ';':
            line->on.success = next;
            line->on.failure = next;
            line->on.start = outer.start;
            outer.start = nullptr;
            break;
        case '|':
            it.advance();
            CZ_DEBUG_ASSERT(it.get() == '|');
            line->on.success = outer.success;
            line->on.failure = next;
            line->on.start = outer.start;
            outer.start = nullptr;
            break;
        case '&':
            if (it.peek(1) == '&') {
                line->on.success = next;
                line->on.failure = outer.failure;
                line->on.start = outer.start;
                outer.start = nullptr;
                it.advance();
            } else {
                line->on.start = next;
            }
            break;
        default:
            CZ_PANIC("unreachable");
        }
        it.advance();

        line = next;
    }
}

///////////////////////////////////////////////////////////////////////////////

static Error parse_pipeline(const Shell_State* shell,
                            cz::Allocator allocator,
                            Parse_Pipeline* out,
                            Text_Iter* it) {
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
        while (!it->at_eob() && cz::is_blank(it->get())) {
            it->advance();
        }

        // Done.
        if (it->at_eob()) {
            break;
        }

        // Get the next word.
        cz::String key = {};
        cz::String word = {};
        bool allow_empty = false;
        bool any_special = false;
        bool special_stderr = false;
        while (!it->at_eob()) {
            switch (it->get()) {
            case CZ_BLANK_CASES:
            case '|':
            case '&':
            case ';':
            case '\n':
                goto endofword;

            case '<':
            case '>': {
                char before = it->before(1);
                if (before == '1') {
                    word.drop(allocator);
                    word = {};
                } else if (before == '2') {
                    special_stderr = true;
                    word.drop(allocator);
                    word = {};
                }
                goto endofword;
            }

            case '\'': {
                allow_empty = true;
                any_special = true;
                it->advance();
                while (1) {
                    if (it->at_eob())
                        return Error_Parse;
                    char c = it->get();
                    it->advance();
                    if (c == '\'')
                        break;
                    word.reserve(allocator, 1);
                    word.push(c);
                }
                break;
            }

            case '"': {
                allow_empty = true;
                any_special = true;
                it->advance();
                while (1) {
                    if (it->at_eob())
                        return Error_Parse;
                    char c = it->get();
                    it->advance();
                    if (c == '"')
                        break;
                    if (c == '$') {
                        cz::Str key;
                        if (get_var_at_point(it, &key)) {
                            cz::Str value;
                            if (get_var(shell, key, &value)) {
                                word.reserve(allocator, value.len);
                                word.append(value);
                            }
                            continue;
                        } else {
                            it->retreat();  // '$'
                        }
                    }
                    if (c == '\\') {
                        if (it->at_eob())
                            return Error_Parse;
                        char c2 = it->get();
                        // From manual testing it looks like these are the only
                        // escape sequences that are processed.  Others are
                        // left (ie typing '\\' -> '\' but '\n' -> '\n').
                        if (c2 == '"' || c2 == '\\' || c2 == '`' || c2 == '$') {
                            c = c2;
                            it->advance();
                        } else if (c2 == '\n') {
                            // Skip backslash newline.
                            it->advance();
                            continue;
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
                it->advance();  // '='
                break;
            }

            case '$': {
                it->advance();  // '$'
                any_special = true;

                cz::Str var;
                if (!get_var_at_point(it, &var)) {
                    it->retreat();  // '$'
                    goto def;
                }

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

            case '~': {
                if (any_special || word.len > 0)
                    goto def;

                char ch = it->peek(1);
                if (ch != '/' && !cz::is_space(ch))
                    goto def;

                cz::Str home;
                if (!get_var(shell, "HOME", &home))
                    goto def;

                any_special = true;
                it->advance();  // '~'
                word.reserve(allocator, home.len + 1);
                word.append(home);
                if (ch == '/') {
                    word.push('/');
                    it->advance();  // '/'
                }
                break;
            }

            case '\\': {
                it->advance();
                char after = it->get();
                // Skip backslash newline.
                if (after == '\n') {
                    it->advance();
                    break;
                }
                // Escape the next character.
                goto def;
            }

            default:
            def:
                word.reserve(allocator, 1);
                word.push(it->get());
                it->advance();
                break;
            }
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
            // Attempt to expand the alias.
            // Note: 'alias a="echo;"; a b' should behave identically to 'echo; b'.
            //       If 'b' is an alias then it should be expanded.
            // Note: 'alias a="echo \""; a b"' should behave identically to 'echo "  b"'.
            cz::Str alias_value;
            if (program.args.len == 0 && get_alias(shell, word, &alias_value)) {
                // Don't double expand the same alias.
                // For example, 'alias a=a' is basically equivalent to
                // having no alias at all instead of infinite looping.
                bool matches = false;
                for (size_t i = 1; i < it->stack.len; ++i) {
                    if (it->stack[i].alias_key == word) {
                        matches = true;
                        break;
                    }
                }

                if (!matches) {
                    // TODO: prevent read after free with 'alias a='alias a=b; echo''.
                    it->push(alias_value, word.clone(cz::heap_allocator()));
                    continue;
                }
            }

            word.reserve_exact(allocator, 1);
            word.null_terminate();
            program.args.reserve(cz::heap_allocator(), 1);
            program.args.push(word);
            continue;
        }

        // "echo $hi" when 'hi' is undefined should have 0 args.
        if (it->at_eob())
            break;

        // Special character.
        switch (it->get()) {
        case '|': {
            if (it->peek(1) == '|')  // '||'
                goto break_outer;

            Error error = finish_program(out, allocator, &program, &state);
            if (error != Error_Success)
                return error;
            it->advance();
        } break;

        case '<':
        case '>': {
            bool is_stdin = it->get() == '<';
            if (is_stdin) {
                state.in_file_i = program.args.len;
            } else if (special_stderr) {
                state.err_file_i = program.args.len;
            } else {
                state.out_file_i = program.args.len;
            }
            it->advance();
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

static bool get_var_at_point(Text_Iter* it, cz::Str* key) {
    if (it->at_eob())
        return false;

    Text_Item* item = &it->stack.last();

    size_t start = item->index;
    for (; item->index < item->text.len; ++item->index) {
        if (!cz::is_alnum(item->text[item->index]))
            break;
    }

    if (start == item->index)
        return false;

    *key = item->text.slice(start, item->index);
    return true;
}
