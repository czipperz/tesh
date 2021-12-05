#include "shell.hpp"

#include <cz/defer.hpp>
#include <cz/heap.hpp>

///////////////////////////////////////////////////////////////////////////////
// Utility
///////////////////////////////////////////////////////////////////////////////

template <class T>
static void change_allocator(cz::Allocator old_allocator,
                             cz::Allocator new_allocator,
                             cz::Vector<T>* vector) {
    auto copy = vector->clone(new_allocator);
    vector->drop(old_allocator);
    *vector = copy;
}

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

static Error parse_program(cz::Allocator allocator,
                           cz::Slice<cz::Str> tokens,
                           Parse_Program* program,
                           size_t* index);

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
        case '>':
            if (*index == *token_start) {
                *any_special = true;
                ++*index;
            }
            return Error_Success;

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

        case '\'':
            ++*index;
            *any_special = true;
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
            break;

            ///////////////////////////////////////////////

        case '"':
            ++*index;
            *any_special = true;
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
            break;

            ///////////////////////////////////////////////

        default:
            ++*index;
            break;
        }
    }
}

static Error advance_through_dollar_sign(cz::Str text, size_t* index) {
    // ${} can stay but $() should be moved to run beforehand.
    CZ_PANIC("todo");
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

    node->type = Shell_Node::SEQUENCE;
    node->v.sequence = sequence.clone(allocator);
    return Error_Success;
}

static Error parse_binary(const Shell_State* shell,
                          cz::Allocator allocator,
                          cz::Slice<cz::Str> tokens,
                          int max_precedence,
                          Shell_Node* node,
                          size_t* index) {
    // Parse left to right: `a && b && c` -> `a && (b && c)`
    // Honor precedence:    `a && b || c` -> `(a && b) || c`

    Shell_Node sub;
    Error error = parse_pipeline(shell, allocator, tokens, &sub, index);
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
        int sub_precedence = cz::min(precedence, max_precedence - 1);

        error = parse_binary(shell, allocator, tokens, sub_precedence, &sub, index);
        if (error != Error_Success)
            return error;
    }
}

static Error parse_pipeline(const Shell_State* shell,
                            cz::Allocator allocator,
                            cz::Slice<cz::Str> tokens,
                            Shell_Node* node,
                            size_t* index) {
    const int max_precedence = 4;

    for (size_t iterations = 0;; ++iterations) {
        Parse_Program program = {};
        Error error = parse_program(allocator, tokens, &program, index);
        if (error != Error_Success)
            return error;

        Shell_Node pnode;
        pnode.type = Shell_Node::PROGRAM;
        pnode.v.program = allocator.clone(program);

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
        change_allocator(cz::heap_allocator(), allocator, &node->v.pipeline);
    }
    return Error_Success;
}

static Error parse_program(cz::Allocator allocator,
                           cz::Slice<cz::Str> tokens,
                           Parse_Program* program,
                           size_t* index) {
    for (; *index < tokens.len; ++*index) {
        // TODO: var=val, <, >
        cz::Str token = tokens[*index];
        if (get_precedence(token))
            break;  // TODO special handling for (???
        if (token[0] == '<' || token[0] == '>')
            CZ_PANIC("todo");
        program->args.reserve(cz::heap_allocator(), 1);
        program->args.push(token);
    }

    if (program->args.len == 0 && program->variable_names.len == 0) {
        return Error_Parse_EmptyProgram;
    }

    change_allocator(cz::heap_allocator(), allocator, &program->args);
    change_allocator(cz::heap_allocator(), allocator, &program->variable_names);
    change_allocator(cz::heap_allocator(), allocator, &program->variable_values);
    return Error_Success;
}
