#include "shell.hpp"

#include <Tracy.hpp>
#include <cz/char_type.hpp>
#include <cz/defer.hpp>
#include <cz/heap.hpp>
#include <cz/string.hpp>

Error parse_line(const Shell_State* shell, cz::Allocator allocator, Parse_Line* out, cz::Str text) {
    ZoneScoped;

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
        cz::String word = {};
        bool allow_empty = false;
        while (index < text.len) {
            switch (text[index]) {
            case CZ_BLANK_CASES:
            case '|':
                goto endofword;

            case '\'': {
                allow_empty = true;
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
                ++index;
                while (1) {
                    if (index == text.len)
                        return Error_Parse;
                    char c = text[index];
                    if (c == '"')
                        break;
                    ++index;
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

            default:
                word.reserve(allocator, 1);
                word.push(text[index]);
                break;
            }
            ++index;
        }

        // Push the word.
    endofword:
        if (allow_empty || word.len > 0) {
            args.reserve(cz::heap_allocator(), 1);
            args.push(word);
            continue;
        }

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

    if (args.len > 0) {
        out->pipeline.reserve(cz::heap_allocator(), 1);
        Parse_Program program = {};
        program.args = args.clone(allocator);
        out->pipeline.push(program);
        args.len = 0;
    }

    return Error_Success;
}
