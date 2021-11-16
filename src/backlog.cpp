#include "backlog.hpp"

#include <cz/heap.hpp>
#include <cz/parse.hpp>
#include "global.hpp"

#define BUFFER_SIZE 4096
#define OUTER_INDEX(index) ((index) >> 12)
#define INNER_INDEX(index) ((index)&0xfff)

char Backlog_State::get(size_t i) {
    CZ_DEBUG_ASSERT(i < length);
    return buffers[OUTER_INDEX(i)][INNER_INDEX(i)];
}

static int64_t append_chunk(Backlog_State* backlog, cz::Str text) {
    if (backlog->length == backlog->max_length)
        return 0;

    uint64_t overhang = INNER_INDEX(backlog->length + text.len);
    uint64_t inner = INNER_INDEX(backlog->length);
    if (overhang < text.len) {
        uint64_t underhang = text.len - overhang;
        if (underhang > 0) {
            memcpy(backlog->buffers.last() + inner, text.buffer + 0, underhang);

            if (backlog->length + underhang == backlog->max_length) {
                text = text.slice_end(underhang);
                goto finish;
            }
        }

        backlog->buffers.reserve(cz::heap_allocator(), 1);
        char* buffer = (char*)cz::heap_allocator().alloc({BUFFER_SIZE, 1});
        CZ_ASSERT(buffer);
        backlog->buffers.push(buffer);

        memcpy(backlog->buffers.last() + 0, text.buffer + underhang, overhang);
    } else {
        memcpy(backlog->buffers.last() + inner, text.buffer, text.len);
    }

finish:
    // Log all the line starts.
    for (size_t i = 0;;) {
        const char* ptr = text.slice_start(i).find('\n');
        if (!ptr)
            break;
        i = ptr - text.buffer + 1;
        backlog->lines.reserve(cz::heap_allocator(), 1);
        backlog->lines.push(backlog->length + i);
    }

    backlog->length += text.len;
    return text.len;
}

static bool ensure_char(Backlog_State* backlog, size_t it, cz::Str fresh, size_t* skip) {
    if (it < backlog->escape_backlog.len)
        return true;
    if (fresh.len <= *skip)
        return false;
    backlog->escape_backlog.reserve(cz::heap_allocator(), 1);
    backlog->escape_backlog.push(fresh[*skip]);
    ++*skip;
    return true;
}

static bool eat_number(Backlog_State* backlog,
                       size_t* it,
                       cz::Str fresh,
                       size_t* skip,
                       int32_t* number) {
    cz::String* text = &backlog->escape_backlog;

    // Load the number.
    size_t end = *it;
    while (1) {
        if (!ensure_char(backlog, end, fresh, skip))
            return false;

        if (!cz::is_digit((*text)[end]))
            break;

        ++end;
    }

    // No number.
    if (*it == end)
        return true;

    // Parse the number.
    uint16_t temp = 0;
    int64_t consumed = cz::parse(text->slice(*it, end), &temp);
    // Overflow means the number is too large.
    if (consumed != end - *it) {
        temp = 32767;
    }

    *number = temp;
    *it = end;
    return true;
}

static bool parse_args(Backlog_State* backlog,
                       size_t* it,
                       cz::Str fresh,
                       size_t* skip,
                       cz::Vector<int32_t>* args) {
    cz::String* text = &backlog->escape_backlog;
    while (1) {
        int32_t arg = -1;
        if (!eat_number(backlog, it, fresh, skip, &arg))
            return false;

        // Args are separated by semicolons.  Also, arguments should
        // default if they are not specified and cut off by a semicolon.
        if (!ensure_char(backlog, *it, fresh, skip))
            return false;
        char semicolon = ((*text)[*it] == ';');

        if (arg == -1 && !semicolon)
            break;
        args->reserve(temp_allocator, 1);
        args->push(arg);

        // Args are separated by semicolons.
        if (!semicolon)
            break;
        ++*it;
    }
    return true;
}

/// Attempt to process an escape sequence.  Returns `true` if it
/// was processed, `false` if we need more input to process it.
static bool process_escape_sequence(Backlog_State* backlog, cz::Str fresh, size_t* skip) {
    // Note(chris.gregory): At least right now we only care
    // about color escape sequences.  The rest we will discard.

    cz::String* text = &backlog->escape_backlog;

    // ESC
    if (!ensure_char(backlog, 0, fresh, skip))
        return false;

    if (!ensure_char(backlog, 1, fresh, skip))
        return false;

    // Ignoring these messages.
    // ESC M = Move up one line.
    // ESC 7 = Save cursor, ESC 8 = Restore cursor.
    // ESC = = Disable numlock, ESC > = Enable numlock.
    // ESC H = Set tabstop at cursor's current column.
    if ((*text)[1] == 'M' || (*text)[1] == '7' || (*text)[1] == '8' || (*text)[1] == '=' ||
        (*text)[1] == '>' || (*text)[1] == 'H') {
        return true;
    }

    if ((*text)[1] == '[') {
        size_t it = 2;

        if (!ensure_char(backlog, 2, fresh, skip))
            return false;

        if ((*text)[2] == '?') {
            it = 3;
            CZ_PANIC("todo");
        } else if ((*text)[2] == '!') {
            it = 3;
            CZ_PANIC("todo");
        } else {
            cz::Vector<int32_t> args = {};
            if (!parse_args(backlog, &it, fresh, skip, &args))
                return false;

            if (!ensure_char(backlog, it, fresh, skip))
                return false;

            // Ignoring these messages (this is probably fine).
            // ESC [ s              Save Cursor
            // ESC [ u              Restore Cursor
            //
            // ESC [ <n> A          Cursor Up
            // ESC [ <n> B          Cursor Down
            // ESC [ <n> C          Cursor Forward
            // ESC [ <n> D          Cursor Backward
            // ESC [ <n> E          Cursor Down Lines
            // ESC [ <n> F          Cursor Up Lines
            // ESC [ <n> G          Cursor Set Column
            // ESC [ <n> d          Cursor Set Row
            // ESC [ <n> l          Cursor Forward to Tabstop
            // ESC [ <n> Z          Cursor Backwards to Tabstop
            // ESC [ <n> S          Scroll Up
            // ESC [ <n> T          Scroll Down
            //
            // ESC [ <m> @          Insert Character (insert n spaces shifting to right text after)
            // ESC [ <m> P          Delete Character (shift left n characters)
            // ESC [ <m> X          Erase Character (overwrite n characters with space)
            // ESC [ <m> L          Insert Line (shift down lines below n times)
            // ESC [ <m> M          Delete Line (delete n lines and shift up lines below them)
            // ESC [ <o> J          Erase in Display (the part specified by o)
            // ESC [ <o> K          Erase in Line (the part specified by o)
            //
            // ESC [ 0 g            Clear Tab Stop at Column
            // ESC [ 3 g            Clear All Tab Stops
            //
            // ESC [ <y> ; <x> H    Cursor Set Position
            // ESC [ <y> ; <x> f    Cursor Set Position
            // ESC [ <n> ; <b> r    Set Scrolling Region between n and b (row numbers).
            if ((*text)[it] == 's' || (*text)[it] == 'u' || (*text)[it] == 'A' ||
                (*text)[it] == 'B' || (*text)[it] == 'C' || (*text)[it] == 'D' ||
                (*text)[it] == 'E' || (*text)[it] == 'F' || (*text)[it] == 'G' ||
                (*text)[it] == 'd' || (*text)[it] == 'l' || (*text)[it] == 'Z' ||
                (*text)[it] == 'S' || (*text)[it] == 'T' || (*text)[it] == '@' ||
                (*text)[it] == 'P' || (*text)[it] == 'X' || (*text)[it] == 'L' ||
                (*text)[it] == 'M' || (*text)[it] == 'J' || (*text)[it] == 'K' ||
                (*text)[it] == 'g' || (*text)[it] == 'H' || (*text)[it] == 'f' ||
                (*text)[it] == 'r') {
                return true;
            }

            // Ignoring these messages (this is probably going to cause bugs).
            // ESC [ 6 n            Report Cursor Position (term should print ESC [ <y> ; <x> R)
            // ESC [ 0 c            Report Device Attributes (term should print ESC [ ? 1 ; 0 c)
            else if ((*text)[it] == 'n' || (*text)[it] == 'c') {
                return true;
            }

            // ESC [ <ns> m         Set Graphic Rendition (series of commands to change
            //                      how future characters are rendered)
            else if ((*text)[it] == 'm') {
                uint64_t graphics_rendition = backlog->graphics_rendition;
                if (args.len == 0)
                    graphics_rendition = (7 << GR_FOREGROUND_SHIFT);

                for (size_t i = 0; i < args.len; ++i) {
                    if (args[i] == 0 || args[i] == -1) {
                        // Reset everything.
                        graphics_rendition = (7 << GR_FOREGROUND_SHIFT);
                    } else if (args[i] == 1) {
                        graphics_rendition |= GR_BOLD;
                    } else if (args[i] == 21) {
                        graphics_rendition &= ~GR_BOLD;
                    } else if (args[i] == 4) {
                        graphics_rendition |= GR_UNDERLINE;
                    } else if (args[i] == 24) {
                        graphics_rendition &= ~GR_UNDERLINE;
                    } else if (args[i] == 7) {
                        graphics_rendition |= GR_REVERSE;
                    } else if (args[i] == 27) {
                        graphics_rendition &= ~GR_REVERSE;
                    } else if ((args[i] >= 30 && args[i] <= 39) ||
                               (args[i] >= 90 && args[i] <= 99)) {
                        // Set foreground color.
                        if (args[i] <= 39)
                            graphics_rendition &= ~GR_BRIGHT;
                        else
                            graphics_rendition |= GR_BRIGHT;
                        graphics_rendition &= ~GR_FOREGROUND_MASK;
                        uint64_t color = args[i] - 30;
                        if (color == 9)
                            color = 7;
                        if (color == 8) {
                            // TODO Parse extended colors.
                            color = 7;
                            CZ_PANIC("todo");
                        }
                        graphics_rendition |= (color << GR_FOREGROUND_SHIFT);
                    } else if ((args[i] >= 40 && args[i] <= 49) ||
                               (args[i] >= 100 && args[i] <= 109)) {
                        // Set background color.
                        if (args[i] <= 49)
                            graphics_rendition &= ~GR_BRIGHT;
                        else
                            graphics_rendition |= GR_BRIGHT;
                        graphics_rendition &= ~GR_BACKGROUND_MASK;
                        uint64_t color = args[i] - 40;
                        if (color == 9)
                            color = 0;
                        if (color == 8) {
                            // TODO Parse extended colors.
                            color = 0;
                            CZ_PANIC("todo");
                        }
                        graphics_rendition |= (color << GR_BACKGROUND_SHIFT);
                    } else {
                        // Ignored.
                    }
                }

                Backlog_Event event = {};
                event.index = backlog->length;
                event.type = BACKLOG_EVENT_SET_GRAPHIC_RENDITION;
                event.payload = graphics_rendition;
                backlog->events.reserve(cz::heap_allocator(), 1);
                backlog->events.push(event);
                backlog->graphics_rendition = graphics_rendition;

                return true;
            }

            // ESC [ UNRECOGNIZED
            else {
                // Undo skipping the UNRECOGNIZED character.
                CZ_DEBUG_ASSERT(*skip > 0);
                --*skip;

#if 0
                // Pass ESC [ as raw.
                append_chunk(backlog, text->slice_end(2));
#else
                append_chunk(backlog, "[");
#endif
                return true;
            }
        }
    } else if ((*text)[1] == '(') {
        CZ_PANIC("todo");
    } else if ((*text)[1] == ')') {
        CZ_PANIC("todo");
    } else {
#if 0
        // The other escape sequences are key values so just pass them through with the escape.
        append_chunk(backlog, "\x1b");
#endif
        return true;
    }
}

int64_t append_text(Backlog_State* backlog, cz::Str text) {
    const char escape = 0x1b;
    int64_t done = 0;

    if (backlog->escape_backlog.len != 0) {
        uint64_t skip = 0;
        if (!process_escape_sequence(backlog, text, &skip)) {
            return text.len;
        }

        backlog->escape_backlog.len = 0;
        text = text.slice_start(skip);
        done += skip;
    }

    while (1) {
        // Find the first special character.
        size_t chunk_len = text.len;
        chunk_len = text.slice_end(chunk_len).find_index('\r');
        chunk_len = text.slice_end(chunk_len).find_index(escape);

        // Append the normal text before it.
        int64_t result = append_chunk(backlog, text.slice_end(chunk_len));
        if (result != chunk_len)
            return done + result;
        done += result;

        if (chunk_len == text.len)
            break;

        // Handle the special character.
        switch (text[chunk_len]) {
        case '\r': {
            // TODO: this isn't right -- this should only move the cursor
            // and not change the line.  But in practice this works.
            size_t outer_before = OUTER_INDEX(backlog->length);
            backlog->length = (backlog->lines.len > 0 ? backlog->lines.last() : 0);
            for (size_t i = outer_before + 1; i-- > OUTER_INDEX(backlog->length) + 1; ++i) {
                cz::heap_allocator().dealloc({backlog->buffers.last(), BUFFER_SIZE});
                backlog->buffers.pop();
            }

            text = text.slice_start(chunk_len + 1);
            done++;
        } break;

        case escape: {
            cz::Str remaining = text.slice_start(chunk_len);
            uint64_t skip = 0;
            if (!process_escape_sequence(backlog, remaining, &skip)) {
                return done + remaining.len;
            }

            backlog->escape_backlog.len = 0;
            text = remaining.slice_start(skip);
            done += skip;
        } break;
        }
    }
    return text.len;
}
