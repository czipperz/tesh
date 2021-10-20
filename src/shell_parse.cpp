#include "shell.hpp"

#include <cz/char_type.hpp>
#include <cz/defer.hpp>
#include <cz/heap.hpp>
#include <cz/string.hpp>

Error parse_line(const Shell_State* shell, cz::Allocator allocator, Parse_Line* out, cz::Str text) {
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
        while (index < text.len) {
            switch (text[index]) {
            case CZ_BLANK_CASES:
            case '|':
                goto endofword;

            default:
                word.reserve(allocator, 1);
                word.push(text[index]);
                break;
            }
            ++index;
        }

        // Push the word.
    endofword:
        if (word.len > 0) {
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
