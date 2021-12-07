#include "shell.hpp"

#include <cz/defer.hpp>
#include <cz/heap.hpp>

///////////////////////////////////////////////////////////////////////////////
// Forward declarations
///////////////////////////////////////////////////////////////////////////////

static Error tokenize(const Shell_State* shell,
                      cz::Allocator allocator,
                      cz::Vector<cz::Str>* tokens,
                      cz::Str text);

static Error advance_through_token(cz::Str text,
                                   size_t* token_start,
                                   size_t* token_end,
                                   bool* any_special,
                                   bool* program_break);
static Error advance_through_single_quote_string(cz::Str text, size_t* index);
static Error advance_through_double_quote_string(cz::Str text, size_t* index);
static Error advance_through_dollar_sign(cz::Str text, size_t* index);

static Error parse_sequence(const Shell_State* shell,
                            cz::Allocator allocator,
                            cz::Slice<cz::Str> tokens,
                            Shell_Node* node,
                            size_t* index);

static Error parse_binary(const Shell_State* shell,
                          cz::Allocator allocator,
                          cz::Slice<cz::Str> tokens,
                          int max_precedence,
                          Shell_Node* node,
                          size_t* index);

static Error parse_pipeline(const Shell_State* shell,
                            cz::Allocator allocator,
                            cz::Slice<cz::Str> tokens,
                            Shell_Node* node,
                            size_t* index);

static Error parse_program(const Shell_State* shell,
                           cz::Allocator allocator,
                           cz::Slice<cz::Str> tokens,
                           Shell_Node* node,
                           size_t* index);
static void deal_with_token(cz::Allocator allocator, Parse_Program* program, cz::Str token);

static cz::Str deref_var_at_point(const Shell_State* shell, cz::Str text, size_t* index);

///////////////////////////////////////////////////////////////////////////////
// Driver
///////////////////////////////////////////////////////////////////////////////

Error parse_script(const Shell_State* shell,
                   cz::Allocator allocator,
                   Shell_Node* root,
                   cz::Str text) {
    cz::Vector<cz::Str> tokens = {};
    CZ_DEFER(tokens.drop(cz::heap_allocator()));
    Error error = tokenize(shell, allocator, &tokens, text);
    if (error != Error_Success)
        return error;

    size_t index = 0;
    return parse_sequence(shell, allocator, tokens, root, &index);
}

///////////////////////////////////////////////////////////////////////////////
// Tokenization
///////////////////////////////////////////////////////////////////////////////

static Error tokenize(const Shell_State* shell,
                      cz::Allocator allocator,
                      cz::Vector<cz::Str>* tokens,
                      cz::Str text) {
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
        if (!any_special && at_start_of_program) {
            cz::Str alias;
            if (get_alias(shell, token, &alias)) {
                // Aliases need to be parsed and merged into the text.  I'm thinking
                // I'll parse tokens in the ALIAS command and just append the tokens.
                // This won't handle `alias="'"`
                CZ_PANIC("todo");
                continue;
            }
        }

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
        case '|':
            if (*index == *token_start) {
                *any_special = true;
                *program_break = true;
                ++*index;
                if (*index < text.len && text[*index - 1] == text[*index]) {
                    ++*index;
                }
            }
            return Error_Success;

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

static Error parse_sequence(const Shell_State* shell,
                            cz::Allocator allocator,
                            cz::Slice<cz::Str> tokens,
                            Shell_Node* node,
                            size_t* index) {
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
        }

        Shell_Node step;
        Error error = parse_binary(shell, allocator, tokens, 8, &step, index);
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

static Error parse_binary(const Shell_State* shell,
                          cz::Allocator allocator,
                          cz::Slice<cz::Str> tokens,
                          int max_precedence,
                          Shell_Node* node,
                          size_t* index) {
    // Parse left to right: `a && b && c` -> `a && (b && c)`
    // Honor precedence:    `a && b || c` -> `(a && b) || c`

    Shell_Node sub;
    Error error;
    if (max_precedence == 6) {
        error = parse_pipeline(shell, allocator, tokens, &sub, index);
    } else {
        error = parse_binary(shell, allocator, tokens, max_precedence - 2, &sub, index);
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
            error = parse_pipeline(shell, allocator, tokens, &sub, index);
        } else {
            error = parse_binary(shell, allocator, tokens, precedence, &sub, index);
        }
        if (error != Error_Success)
            return error;
    }
}

///////////////////////////////////////////////////////////////////////////////
// Parse pipeline
///////////////////////////////////////////////////////////////////////////////

static Error parse_pipeline(const Shell_State* shell,
                            cz::Allocator allocator,
                            cz::Slice<cz::Str> tokens,
                            Shell_Node* node,
                            size_t* index) {
    const int max_precedence = 4;

    for (size_t iterations = 0;; ++iterations) {
        Shell_Node pnode;
        Error error = parse_program(shell, allocator, tokens, &pnode, index);
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

static Error parse_program(const Shell_State* shell,
                           cz::Allocator allocator,
                           cz::Slice<cz::Str> tokens,
                           Shell_Node* node,
                           size_t* index) {
    Parse_Program program = {};

    size_t start = *index;
    for (; *index < tokens.len;) {
        cz::Str token = tokens[*index];
        if (get_precedence(token)) {
            if (token == "(") {
                if (*index > start) {
                    return Error_Parse_UnterminatedProgram;
                } else {
                    ++*index;
                    Error error = parse_sequence(shell, allocator, tokens, node, index);
                    if (error != Error_Success)
                        return error;
                    if (*index >= tokens.len || tokens[*index] != ")")
                        return Error_Parse_UnterminatedParen;
                    ++*index;
                    return Error_Success;
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

        deal_with_token(allocator, &program, token);
        ++*index;
    }

    if (program.args.len == 0 && program.variable_names.len == 0) {
        return Error_Parse_EmptyProgram;
    }

    cz::change_allocator(cz::heap_allocator(), allocator, &program.args);
    cz::change_allocator(cz::heap_allocator(), allocator, &program.variable_names);
    cz::change_allocator(cz::heap_allocator(), allocator, &program.variable_values);

    node->type = Shell_Node::PROGRAM;
    node->v.program = allocator.clone(program);
    return Error_Success;
}

static void deal_with_token(cz::Allocator allocator, Parse_Program* program, cz::Str token) {
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
            if (any_special || program->args.len > 0)
                goto def;

            cz::Str key = token.slice_end(index);
            cz::Str value = token.slice_start(index + 1);
            program->variable_names.reserve(cz::heap_allocator(), 1);
            program->variable_values.reserve(cz::heap_allocator(), 1);
            program->variable_names.push(key);
            program->variable_values.push(value);
            return;
        }

        def:
        default:
            ++index;
            break;
        }
    }

    program->args.reserve(cz::heap_allocator(), 1);
    program->args.push(token);
}

///////////////////////////////////////////////////////////////////////////////
// Argument expansion
///////////////////////////////////////////////////////////////////////////////

void expand_arg(const Shell_State* shell,
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
                    cz::Str value = deref_var_at_point(shell, text, &index);
                    word->reserve(allocator, value.len);
                    word->append(value);
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
            cz::Str value = deref_var_at_point(shell, text, &index);
            if (words) {
                size_t i = 0;
                while (i < value.len) {
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
            } else {
                word->reserve(allocator, value.len);
                word->append(value);
            }
        } break;

        case '~': {
            if (index == 0) {
                cz::Str value;
                if (get_var(shell, "HOME", &value)) {
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

void expand_arg_single(const Shell_State* shell,
                       cz::Str text,
                       cz::Allocator allocator,
                       cz::String* word) {
    expand_arg(shell, text, allocator, nullptr, word);
}

void expand_arg_split(const Shell_State* shell,
                      cz::Str text,
                      cz::Allocator allocator,
                      cz::Vector<cz::Str>* output) {
    cz::String word = {};
    expand_arg(shell, text, allocator, output, &word);
}

static cz::Str deref_var_at_point(const Shell_State* shell, cz::Str text, size_t* index) {
    ++*index;
    if (*index == text.len)
        return "$";

    switch (text[*index]) {
    case CZ_ALNUM_CASES:
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
        get_var(shell, text.slice(start, *index), &value);
        return value;
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
        get_var(shell, text.slice(start, *index), &value);
        ++*index;
        return value;
    } break;

    default:
        return "$";
    }
}
