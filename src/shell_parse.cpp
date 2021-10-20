#include "shell_parse.hpp"

#include <cz/char_type.hpp>
#include <cz/defer.hpp>
#include <cz/heap.hpp>
#include <cz/string.hpp>

Error parse_line(const Shell_State* shell, cz::Allocator allocator, Shell_Line* out, cz::Str text) {
    cz::Vector<cz::Str> words = {};
    CZ_DEFER(words.drop(cz::heap_allocator()));

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
        words.reserve(cz::heap_allocator(), 1);
        words.push(word);
    }

    if (words.len > 0) {
        out->pipeline.reserve(cz::heap_allocator(), 1);
        Shell_Program program = {};
        program.words = words.clone(allocator);
        out->pipeline.push(program);
        words.len = 0;
    }

    return ERROR_SUCCESS;
}
