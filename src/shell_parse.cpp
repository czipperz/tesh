#include "shell.hpp"

#include <cz/char_type.hpp>
#include <cz/defer.hpp>
#include <cz/format.hpp>
#include <cz/heap.hpp>
#include <cz/parse.hpp>

#include "global.hpp"

///////////////////////////////////////////////////////////////////////////////
// Forward declarations
///////////////////////////////////////////////////////////////////////////////

static Error tokenize(cz::Allocator allocator, cz::Vector<cz::Str>* tokens, cz::Str text);

static Error advance_through_token(cz::Str text,
                                   size_t* token_start,
                                   size_t* token_end,
                                   bool* any_special,
                                   bool* program_break);
static Error advance_through_single_quote_string(cz::Str text, size_t* index);
static Error advance_through_double_quote_string(cz::Str text, size_t* index);
static Error advance_through_dollar_sign(cz::Str text, size_t* index);

static Error parse_sequence(cz::Allocator allocator,
                            bool force_alloc,
                            cz::Slice<cz::Str> tokens,
                            Shell_Node* node,
                            size_t* index,
                            cz::Slice<cz::Str> terminators);

static Error parse_binary(cz::Allocator allocator,
                          bool force_alloc,
                          cz::Slice<cz::Str> tokens,
                          int max_precedence,
                          Shell_Node* node,
                          size_t* index);

static Error parse_pipeline(cz::Allocator allocator,
                            bool force_alloc,
                            cz::Slice<cz::Str> tokens,
                            Shell_Node* node,
                            size_t* index);

static Error parse_program(cz::Allocator allocator,
                           bool force_alloc,
                           cz::Slice<cz::Str> tokens,
                           Shell_Node* node,
                           size_t* index);
static Error deal_with_token(cz::Allocator allocator,
                             bool force_alloc,
                             Parse_Program* program,
                             cz::Str token);

static Error parse_if(cz::Allocator allocator,
                      bool force_alloc,
                      cz::Slice<cz::Str> tokens,
                      Shell_Node* node,
                      size_t* index);
static Error parse_function_declaration(cz::Allocator allocator,
                                        bool force_alloc,
                                        cz::Slice<cz::Str> tokens,
                                        Shell_Node* node,
                                        size_t* index,
                                        cz::Str name);

static void deref_var_at_point(const Shell_Local* local,
                               cz::Str text,
                               size_t* index,
                               cz::Vector<cz::Str>* outputs);

///////////////////////////////////////////////////////////////////////////////
// Driver
///////////////////////////////////////////////////////////////////////////////

Error parse_script(cz::Allocator allocator, Shell_Node* root, cz::Str text) {
    cz::Vector<cz::Str> tokens = {};
    CZ_DEFER(tokens.drop(cz::heap_allocator()));
    Error error = tokenize(allocator, &tokens, text);
    if (error != Error_Success)
        return error;

    size_t index = 0;

    return parse_sequence(allocator, /*force_alloc=*/false, tokens, root, &index, {});
}

///////////////////////////////////////////////////////////////////////////////
// Tokenization
///////////////////////////////////////////////////////////////////////////////

static Error tokenize(cz::Allocator allocator, cz::Vector<cz::Str>* tokens, cz::Str text) {
    size_t index = 0;
    bool at_start_of_program = true;
    while (1) {
        size_t token_start = index;
        size_t token_end = index;
        bool any_special = false;
        bool program_break = false;
        Error error =
            advance_through_token(text, &token_start, &token_end, &any_special, &program_break);
        if (error != Error_Success)
            return error;
        if (token_start == token_end)
            break;

        cz::Str token = text.slice(token_start, token_end);
        tokens->reserve(cz::heap_allocator(), 1);
        tokens->push(token);
        index = token_end;
        at_start_of_program = program_break;
    }
    return Error_Success;
}

///////////////////////////////////////////////////////////////////////////////
// Tokenization - eat one token
///////////////////////////////////////////////////////////////////////////////

static Error advance_through_token(cz::Str text,
                                   size_t* token_start,
                                   size_t* index,
                                   bool* any_special,
                                   bool* program_break) {
    // Skip starting whitespace.
    for (; *index < text.len; ++*index) {
        if (!cz::is_blank(text[*index]))
            break;
    }

    *token_start = *index;
    while (1) {
        if (*index >= text.len)
            return Error_Success;

        switch (text[*index]) {
        case CZ_BLANK_CASES:
            return Error_Success;

        case '\n':
        case ';':
            if (*index == *token_start) {
                *any_special = true;
                *program_break = true;
                ++*index;
            }
            return Error_Success;

        case '<':  // TODO <& >&, <<, <<<, etc.
        case '>': {
            cz::Str before = text.slice(*token_start, *index);
            if (*index == *token_start || before == "1" || before == "2") {
                *any_special = true;
                ++*index;
            }
            return Error_Success;
        }

        case '&':
        case '|': {
            if (*index == *token_start) {
                *any_special = true;
                *program_break = true;
                ++*index;
                if (*index < text.len && text[*index - 1] == text[*index]) {
                    ++*index;
                }
            }
            return Error_Success;
        }

            ///////////////////////////////////////////////

        case '#': {
            // Treat '#' as normal character in middle of token.
            if (*index != *token_start) {
                ++*index;
                break;
            }

            // Ignore to end of line.
            for (++*index; *index < text.len; ++*index) {
                if (text[*index] == '\n') {
                    break;
                }
            }

            // Look for the next token.
            *token_start = *index;
            break;
        }

        ///////////////////////////////////////////////

#if 0  // TODO
        case '{':
            ++*index;
            has_open_curly = 1;
            break;
        case ',':
            ++*index;
            if (has_open_curly)
                has_open_curly = 2;
        case '}':
            ++*index;
            if (has_open_curly == 2)
                *has_curlies = true;
            break;
#endif

            ///////////////////////////////////////////////

        case '(':
        case ')':
            if (*index == *token_start) {
                *any_special = true;
                *program_break = true;
                ++*index;
            }
            return Error_Success;

            ///////////////////////////////////////////////

        case '\'': {
            *any_special = true;
            Error error = advance_through_single_quote_string(text, index);
            if (error != Error_Success)
                return error;
        } break;

        case '"': {
            *any_special = true;
            Error error = advance_through_double_quote_string(text, index);
            if (error != Error_Success)
                return error;
        } break;

        case '$': {
            *any_special = true;
            Error error = advance_through_dollar_sign(text, index);
            if (error != Error_Success)
                return error;
        } break;

            ///////////////////////////////////////////////

        case '\\': {
            ++*index;
            if (*index < text.len)
                ++*index;
        } break;

        default:
            ++*index;
            break;
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
// Tokenization utility
///////////////////////////////////////////////////////////////////////////////

static Error advance_through_single_quote_string(cz::Str text, size_t* index) {
    ++*index;
    while (1) {
        if (*index == text.len)
            return Error_Parse_UnterminatedString;

        // Go until we hit another '\''.
        if (text[*index] == '\'') {
            ++*index;
            break;
        }
        ++*index;
    }
    return Error_Success;
}

static Error advance_through_double_quote_string(cz::Str text, size_t* index) {
    ++*index;
    while (1) {
        if (*index == text.len)
            return Error_Parse_UnterminatedString;

        // Ignore the next character indiscriminately.
        if (text[*index] == '\\') {
            ++*index;
            if (*index == text.len)
                return Error_Parse_UnterminatedString;
            ++*index;
            continue;
        }

        if (text[*index] == '$') {
            Error error = advance_through_dollar_sign(text, index);
            if (error != Error_Success)
                return error;
        }

        // Go until we hit another '"'.
        if (text[*index] == '"') {
            ++*index;
            break;
        }
        ++*index;
    }
    return Error_Success;
}

static Error advance_through_dollar_sign(cz::Str text, size_t* index) {
    ++*index;
    if (*index == text.len)
        return Error_Success;

    switch (text[*index]) {
    case CZ_ALNUM_CASES:
    case '_': {
        ++*index;
        while (*index < text.len) {
            char ch = text[*index];
            if (!cz::is_alnum(ch) || ch == '_')
                break;
            ++*index;
        }
    } break;

    case '{': {
        ++*index;
        size_t start = *index;
        while (*index < text.len) {
            char ch = text[*index];
            if (!cz::is_alnum(ch) || ch == '_')
                break;
            ++*index;
        }
        if (*index == start || *index == text.len || text[*index] != '}') {
            // TODO handle cases like ${a:b}
            return Error_Parse_UnterminatedVariable;
        }
        ++*index;
    } break;

    case '@': {
        ++*index;
    } break;

    default:
        // ${} can stay but $() should be moved to run beforehand.
        CZ_PANIC("todo");
    }

    return Error_Success;
}

///////////////////////////////////////////////////////////////////////////////
// Parse
///////////////////////////////////////////////////////////////////////////////

// File is broken by highest level then pieces are broken into lower precedence groups.
static int get_precedence(cz::Str token) {
    if (token == ")")
        return 12;
    if (token == ";" || token == "\n" || token == "&")
        return 10;
    if (token == "||")
        return 8;
    if (token == "&&")
        return 6;
    if (token == "|")
        return 4;
    if (token == "(")
        return 2;
    return 0;
}

///////////////////////////////////////////////////////////////////////////////
// Parse sequence
///////////////////////////////////////////////////////////////////////////////

static Error parse_sequence(cz::Allocator allocator,
                            bool force_alloc,
                            cz::Slice<cz::Str> tokens,
                            Shell_Node* node,
                            size_t* index,
                            cz::Slice<cz::Str> terminators) {
    const int max_precedence = 10;

    cz::Vector<Shell_Node> sequence = {};
    CZ_DEFER(sequence.drop(cz::heap_allocator()));

    while (1) {
        if (*index == tokens.len)
            break;
        cz::Str token = tokens[*index];
        int precedence = get_precedence(token);
        if (precedence > max_precedence)
            break;

        if (token == ";" || token == "\n") {
            ++*index;
            continue;
        }

        if (token == "&") {
            if (sequence.len == 0) {
                return Error_Parse_EmptyProgram;
            }
            sequence.last().async = true;
            ++*index;
            continue;
        }

        // This is used for parsing 'if' / 'for' / 'while' statements.
        if (terminators.contains(token)) {
            break;
        }

        Shell_Node step;
        Error error = parse_binary(allocator, force_alloc, tokens, 8, &step, index);
        if (error != Error_Success)
            return error;

        sequence.reserve(cz::heap_allocator(), 1);
        sequence.push(step);
    }

    if (sequence.len == 1) {
        *node = sequence[0];
    } else {
        node->type = Shell_Node::SEQUENCE;
        node->v.sequence = sequence.clone(allocator);
    }
    return Error_Success;
}

///////////////////////////////////////////////////////////////////////////////
// Parse binary
///////////////////////////////////////////////////////////////////////////////

static Error parse_binary(cz::Allocator allocator,
                          bool force_alloc,
                          cz::Slice<cz::Str> tokens,
                          int max_precedence,
                          Shell_Node* node,
                          size_t* index) {
    // Parse left to right: `a && b && c` -> `a && (b && c)`
    // Honor precedence:    `a && b || c` -> `(a && b) || c`

    Shell_Node sub;
    Error error;
    if (max_precedence == 6) {
        error = parse_pipeline(allocator, force_alloc, tokens, &sub, index);
    } else {
        error = parse_binary(allocator, force_alloc, tokens, max_precedence - 2, &sub, index);
    }
    if (error != Error_Success)
        return error;

    while (1) {
        if (*index == tokens.len) {
        stop:
            *node = sub;
            return Error_Success;
        }

        cz::Str token = tokens[*index];
        int precedence = get_precedence(token);
        if (precedence > max_precedence) {
            goto stop;
        }
        ++*index;

        // Recurse on the right side.
        Shell_Node* leftp = allocator.clone(sub);
        Shell_Node* rightp = allocator.alloc<Shell_Node>();
        *rightp = {};
        node->type = (precedence == 6 ? Shell_Node::AND : Shell_Node::OR);
        node->v.binary.left = leftp;
        node->v.binary.right = rightp;
        node = rightp;

        // Since we're doing LTR loop here, there's no point in
        // having a child do it as well at the same precedence.
        if (precedence == max_precedence) {
            error = parse_pipeline(allocator, force_alloc, tokens, &sub, index);
        } else {
            error = parse_binary(allocator, force_alloc, tokens, precedence, &sub, index);
        }
        if (error != Error_Success)
            return error;
    }
}

///////////////////////////////////////////////////////////////////////////////
// Parse pipeline
///////////////////////////////////////////////////////////////////////////////

static Error parse_pipeline(cz::Allocator allocator,
                            bool force_alloc,
                            cz::Slice<cz::Str> tokens,
                            Shell_Node* node,
                            size_t* index) {
    const int max_precedence = 4;

    for (size_t iterations = 0;; ++iterations) {
        Shell_Node pnode;
        Error error = parse_program(allocator, force_alloc, tokens, &pnode, index);
        if (error != Error_Success)
            return error;

        // Delay making the program into a pipeline.
        if (iterations == 0) {
            *node = pnode;
        } else if (iterations == 1) {
            Shell_Node first = *node;
            node->type = Shell_Node::PIPELINE;
            node->v.pipeline = {};
            node->v.pipeline.reserve(cz::heap_allocator(), 2);
            node->v.pipeline.push(first);
            node->v.pipeline.push(pnode);
        } else {
            node->v.pipeline.reserve(cz::heap_allocator(), 1);
            node->v.pipeline.push(pnode);
        }

        if (*index == tokens.len)
            break;

        // Handle meta token.
        cz::Str token = tokens[*index];
        if (token == "|") {
            ++*index;
            continue;
        }

        CZ_DEBUG_ASSERT(get_precedence(token) > max_precedence);
        break;
    }

    if (node->type == Shell_Node::PIPELINE) {
        cz::change_allocator(cz::heap_allocator(), allocator, &node->v.pipeline);
    }
    return Error_Success;
}

///////////////////////////////////////////////////////////////////////////////
// Parse program
///////////////////////////////////////////////////////////////////////////////

static Error parse_program(cz::Allocator allocator,
                           bool force_alloc,
                           cz::Slice<cz::Str> tokens,
                           Shell_Node* node,
                           size_t* index) {
    if (*index < tokens.len) {
        if (tokens[*index] == "if") {
            return parse_if(allocator, force_alloc, tokens, node, index);
        }
    }

    Parse_Program program = {};

    for (; *index < tokens.len;) {
        cz::Str token = tokens[*index];
        if (get_precedence(token)) {
            if (token == "(") {
                if (!program.is_sub && program.v.args.len == 1) {
                    // Function declaration
                    return parse_function_declaration(allocator, force_alloc, tokens, node, index,
                                                      program.v.args[0]);
                } else if (program.is_sub || program.v.args.len > 0) {
                    return Error_Parse_UnterminatedProgram;
                } else {
                    ++*index;
                    Shell_Node inner;
                    Error error = parse_sequence(allocator, force_alloc, tokens, &inner, index, {});
                    if (error != Error_Success)
                        return error;
                    if (*index >= tokens.len || tokens[*index] != ")")
                        return Error_Parse_UnterminatedParen;

                    program.is_sub = true;
                    program.v.sub = allocator.clone(inner);
                    ++*index;
                    continue;
                }
            }

            break;
        }

        if (token == "<" || token == ">" || token == "1>" || token == "2>") {
            if (*index + 1 == tokens.len)
                return Error_Parse_NothingToIndirect;
            cz::Str* slot;
            if (token == "<") {
                slot = &program.in_file;
            } else if (token == ">" || token == "1>") {
                slot = &program.out_file;
            } else if (token == "2>") {
                slot = &program.err_file;
            } else {
                CZ_PANIC("unreachable");
            }

            if (!slot->buffer)
                *slot = tokens[*index + 1];
            *index += 2;
            continue;
        }

        Error error = deal_with_token(allocator, force_alloc, &program, token);
        if (error != Error_Success)
            return error;
        ++*index;
    }

    if (!program.is_sub && program.v.args.len == 0 && program.variable_names.len == 0) {
        return Error_Parse_EmptyProgram;
    }

    if (!program.is_sub)
        cz::change_allocator(cz::heap_allocator(), allocator, &program.v.args);
    cz::change_allocator(cz::heap_allocator(), allocator, &program.variable_names);
    cz::change_allocator(cz::heap_allocator(), allocator, &program.variable_values);

    node->type = Shell_Node::PROGRAM;
    node->v.program = allocator.clone(program);
    return Error_Success;
}

static Error deal_with_token(cz::Allocator allocator,
                             bool force_alloc,
                             Parse_Program* program,
                             cz::Str token) {
    bool any_special = false;
    for (size_t index = 0; index < token.len;) {
        switch (token[index]) {
        case '\'': {
            any_special = true;
            Error error = advance_through_single_quote_string(token, &index);
            CZ_ASSERT(error == Error_Success);
        } break;

        case '"': {
            any_special = true;
            Error error = advance_through_double_quote_string(token, &index);
            CZ_ASSERT(error == Error_Success);
        } break;

        case '$': {
            any_special = true;
            Error error = advance_through_dollar_sign(token, &index);
            CZ_ASSERT(error == Error_Success);
        } break;

        case '=': {
            if (any_special || program->is_sub || program->v.args.len > 0)
                goto def;

            cz::Str key = token.slice_end(index);
            cz::Str value = token.slice_start(index + 1);
            program->variable_names.reserve(cz::heap_allocator(), 1);
            program->variable_values.reserve(cz::heap_allocator(), 1);
            program->variable_names.push(key);
            program->variable_values.push(value);
            return Error_Success;
        }

        def:
        default:
            ++index;
            break;
        }
    }

    if (program->is_sub) {
        // '(inner) outer' invalid.
        return Error_Parse_UnterminatedProgram;
    }

    if (force_alloc)
        token = token.clone_null_terminate(allocator);

    program->v.args.reserve(cz::heap_allocator(), 1);
    program->v.args.push(token);
    return Error_Success;
}

///////////////////////////////////////////////////////////////////////////////
// Argument expansion
///////////////////////////////////////////////////////////////////////////////

void expand_arg(const Shell_Local* local,
                cz::Str text,
                cz::Allocator allocator,
                cz::Vector<cz::Str>* words,
                cz::String* word) {
    bool has_word = false;
    for (size_t index = 0; index < text.len;) {
        switch (text[index]) {
        case '\'': {
            has_word = true;
            ++index;
            while (1) {
                if (index == text.len)
                    CZ_PANIC("Error_Parse_UnterminatedString should be caught earlier");

                // Go until we hit another '\''.
                if (text[index] == '\'') {
                    ++index;
                    break;
                }

                word->reserve(allocator, 1);
                word->push(text[index]);
                ++index;
            }
        } break;

        case '"': {
            has_word = true;
            ++index;
            while (1) {
                if (index == text.len)
                    CZ_PANIC("Error_Parse_UnterminatedString should be caught earlier");

                // Go until we hit another '"'.
                if (text[index] == '"') {
                    ++index;
                    break;
                } else if (text[index] == '$') {
                    size_t index_before = index;
                    cz::Vector<cz::Str> values = {};
                    CZ_DEFER(values.drop(cz::heap_allocator()));
                    deref_var_at_point(local, text, &index, &values);

                    if (words && index_before == 1 && text[index] == '"') {
                        // "$@" -> "$1" "$2" "$3" ...
                        CZ_DEBUG_ASSERT(word->len == 0);
                        words->reserve(cz::heap_allocator(), values.len);
                        words->append(values);
                    } else if (values.len == 1) {
                        // "$x" -> "$x"
                        word->reserve(allocator, values[0].len);
                        word->append(values[0]);
                    } else {
                        // "a$@" -> "a$1 $2 $3 ..."
                        // x="$@" -> x="$1 $2 $3 ..."
                        for (size_t i = 0; i < values.len; ++i) {
                            if (i >= 1)
                                word->push(' ');
                            word->reserve(allocator, values[i].len + 1);
                            word->append(values[i]);
                        }
                    }

                    continue;
                } else if (text[index] == '\\') {
                    ++index;
                    if (index == text.len)
                        CZ_PANIC("Error_Parse_UnterminatedString should be caught earlier");
                    char c2 = text[index];

                    // From manual testing it looks like these are the only
                    // escape sequences that are processed.  Others are
                    // left (ie typing '\\' -> '\' but '\n' -> '\n').
                    if (c2 == '"' || c2 == '\\' || c2 == '`' || c2 == '$') {
                        word->reserve(allocator, 1);
                        word->push(c2);
                        ++index;
                    } else if (c2 == '\n') {
                        // Skip backslash newline.
                        ++index;
                    } else {
                        // Pass through both the backslash and the character afterwords.
                        word->reserve(allocator, 1);
                        word->push('\\');
                    }
                    continue;
                }

                word->reserve(allocator, 1);
                word->push(text[index]);
                ++index;
            }
        } break;

        case '$': {
            cz::Vector<cz::Str> values = {};
            CZ_DEFER(values.drop(cz::heap_allocator()));
            deref_var_at_point(local, text, &index, &values);

            if (words) {
                for (size_t v = 0; v < values.len; ++v) {
                    cz::Str value = values[v];

                    // Break up arguments.
                    if (v > 0 && (has_word || word->len > 0)) {
                        words->reserve(cz::heap_allocator(), 1);
                        words->push(*word);
                        *word = {};
                        has_word = false;
                    }

                    for (size_t i = 0; i < value.len;) {
                        if (cz::is_blank(value[i])) {
                            if (has_word || word->len > 0) {
                                words->reserve(cz::heap_allocator(), 1);
                                words->push(*word);
                                *word = {};
                                has_word = false;
                            }
                            ++i;
                        }
                        for (; i < value.len; ++i) {
                            if (!cz::is_blank(value[i]))
                                break;
                        }

                        for (; i < value.len; ++i) {
                            if (cz::is_blank(value[i]))
                                break;
                            word->reserve(allocator, 1);
                            word->push(value[i]);
                        }
                    }
                }
            } else {
                for (size_t v = 0; v < values.len; ++v) {
                    if (v >= 1)
                        word->push(' ');
                    word->reserve(allocator, values[v].len + 1);
                    word->append(values[v]);
                }
            }
        } break;

        case '~': {
            if (index == 0) {
                cz::Str value;
                if (get_var(local, "HOME", &value)) {
                    word->reserve(allocator, value.len);
                    word->append(value);
                }
            } else {
                word->reserve(allocator, 1);
                word->push('~');
            }
            ++index;
        } break;

        case '\\': {
            ++index;
            if (index < text.len) {
                char c2 = text[index];
                if (c2 == '"' || c2 == '\\' || c2 == '`' || c2 == '$' || c2 == ' ' || c2 == '~') {
                    word->reserve(allocator, 1);
                    word->push(c2);
                    ++index;
                } else if (c2 == '\n') {
                    // Skip backslash newline.
                    ++index;
                } else {
                    word->reserve(allocator, 1);
                    word->push('\\');
                }
            } else {
                word->reserve(allocator, 1);
                word->push('\\');
            }
        } break;

        default:
            word->reserve(allocator, 1);
            word->push(text[index]);
            ++index;
            break;
        }
    }

    if (words && (has_word || word->len > 0)) {
        words->reserve(cz::heap_allocator(), 1);
        words->push(*word);
        *word = {};
        has_word = false;
    }
}

void expand_arg_single(const Shell_Local* local,
                       cz::Str text,
                       cz::Allocator allocator,
                       cz::String* word) {
    expand_arg(local, text, allocator, nullptr, word);
}

void expand_arg_split(const Shell_Local* local,
                      cz::Str text,
                      cz::Allocator allocator,
                      cz::Vector<cz::Str>* output) {
    cz::String word = {};
    expand_arg(local, text, allocator, output, &word);
}

static void deref_var_at_point(const Shell_Local* local,
                               cz::Str text,
                               size_t* index,
                               cz::Vector<cz::Str>* outputs) {
    outputs->reserve(cz::heap_allocator(), 1);

    ++*index;
    if (*index == text.len) {
        outputs->push("$");
        return;
    }

    switch (text[*index]) {
    case CZ_ALPHA_CASES:
    case '_': {
        size_t start = *index;
        ++*index;
        while (*index < text.len) {
            char ch = text[*index];
            if (!(cz::is_alnum(ch) || ch == '_'))
                break;
            ++*index;
        }
        cz::Str value = "";
        get_var(local, text.slice(start, *index), &value);
        outputs->push(value);
    } break;

    case '{': {
        ++*index;
        size_t start = *index;
        while (*index < text.len) {
            char ch = text[*index];
            if (!cz::is_alnum(ch) || ch == '_')
                break;
            ++*index;
        }
        if (*index == start || *index == text.len || text[*index] != '}') {
            CZ_PANIC("Error_Parse_UnterminatedVariable should be handled earlier");
        }

        cz::Str value = "";
        get_var(local, text.slice(start, *index), &value);
        ++*index;
        outputs->push(value);
    } break;

    case '@': {
        ++*index;
        if (local->args.len == 0)
            break;
        outputs->reserve(cz::heap_allocator(), local->args.len - 1);
        outputs->append(local->args.slice_start(1));
    } break;

    case '*': {
        cz::String string = {};
        for (size_t i = 1; i < local->args.len; ++i) {
            if (i >= 2)
                string.push(' ');
            string.reserve(temp_allocator, local->args[i].len + 1);
            string.append(local->args[i]);
        }
        outputs->push(string);
    } break;

    case '#': {
        outputs->push(cz::format(temp_allocator, cz::max(local->args.len, (size_t)1) - 1));
    } break;

    case CZ_DIGIT_CASES: {
        size_t i = 0;
        int64_t eat = cz::parse(text.slice_start(*index), &i);
        if (eat < 0) {
            eat = -eat;
            i = local->args.len;
        }
        *index += eat;

        if (i < local->args.len) {
            outputs->push(local->args[i]);
        }
    } break;

    default:
        outputs->push("$");
        return;
    }
}

static Error parse_if(cz::Allocator allocator,
                      bool force_alloc,
                      cz::Slice<cz::Str> tokens,
                      Shell_Node* node,
                      size_t* index) {
    ++*index;

    Shell_Node cond = {};
    Error error = parse_binary(allocator, force_alloc, tokens, 8, &cond, index);
    if (error != Error_Success)
        return error;

    if (*index == tokens.len)
        return Error_Parse_UnterminatedIf;
    if (tokens[*index] == "&") {
        cond.async = true;
    } else {
        CZ_DEBUG_ASSERT(tokens[*index] == ";" || tokens[*index] == "\n");
    }
    ++*index;

    if (*index == tokens.len)
        return Error_Parse_UnterminatedIf;
    if (tokens[*index] != "then")
        return Error_Parse_UnterminatedIf;
    ++*index;

    cz::Str terminators[] = {"fi"};
    Shell_Node then = {};
    error = parse_sequence(allocator, force_alloc, tokens, &then, index, terminators);
    if (error != Error_Success)
        return error;

    if (*index == tokens.len)
        return Error_Parse_UnterminatedIf;
    CZ_DEBUG_ASSERT(tokens[*index] == "fi");
    ++*index;

    *node = {};
    node->type = Shell_Node::IF;
    node->v.if_.cond = allocator.clone(cond);
    node->v.if_.then = allocator.clone(then);
    node->v.if_.other = nullptr;

    return Error_Success;
}

static Error parse_function_declaration(cz::Allocator allocator,
                                        bool force_alloc,
                                        cz::Slice<cz::Str> tokens,
                                        Shell_Node* node,
                                        size_t* index,
                                        cz::Str name) {
    // TODO: the function needs to be nearly permanently allocated
    // because it is used beyond the end of the pipeline.
    allocator = permanent_allocator;

    if (*index == tokens.len)
        return Error_Parse_UnterminatedFunctionDeclaration;
    if (tokens[*index] != "(")
        return Error_Parse_UnterminatedFunctionDeclaration;
    ++*index;

    if (*index == tokens.len)
        return Error_Parse_UnterminatedFunctionDeclaration;
    if (tokens[*index] != ")")
        return Error_Parse_UnterminatedFunctionDeclaration;
    ++*index;

    if (*index == tokens.len)
        return Error_Parse_UnterminatedFunctionDeclaration;
    if (tokens[*index] != "{")
        return Error_Parse_UnterminatedFunctionDeclaration;
    ++*index;

    cz::Str terminators[] = {"}"};
    Shell_Node body;
    Error error =
        parse_sequence(allocator, /*force_alloc=*/true, tokens, &body, index, terminators);
    if (error != Error_Success)
        return error;

    if (*index == tokens.len)
        return Error_Parse_UnterminatedFunctionDeclaration;
    if (tokens[*index] != "}")
        return Error_Parse_UnterminatedFunctionDeclaration;
    ++*index;

    *node = {};
    node->type = Shell_Node::FUNCTION;
    node->v.function.name = name;
    node->v.function.body = allocator.clone(body);

    return Error_Success;
}
