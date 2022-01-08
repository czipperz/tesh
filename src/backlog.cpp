#include "backlog.hpp"

#include <cz/heap.hpp>
#include <cz/parse.hpp>
#include "global.hpp"

///////////////////////////////////////////////////////////////////////////////
// Module Configuration
///////////////////////////////////////////////////////////////////////////////

#define OUTER_INDEX(index) ((index) >> 12)
#define INNER_INDEX(index) ((index)&0xfff)

///////////////////////////////////////////////////////////////////////////////
// Forward Declarations
///////////////////////////////////////////////////////////////////////////////

static void truncate_to(Backlog_State* backlog, uint64_t new_length);

///////////////////////////////////////////////////////////////////////////////
// Module Code
///////////////////////////////////////////////////////////////////////////////

void cleanup_backlog(cz::Slice<Backlog_State*> backlogs, Backlog_State* backlog) {
    CZ_DEBUG_ASSERT(backlog->refcount == 0);
    backlogs[backlog->id] = nullptr;
    for (size_t i = 0; i < backlog->buffers.len; ++i) {
        char* buffer = backlog->buffers[i];
        cz::heap_allocator().dealloc({buffer, BACKLOG_BUFFER_SIZE});
    }
    backlog->buffers.drop(cz::heap_allocator());
    backlog->lines.drop(cz::heap_allocator());
    backlog->events.drop(cz::heap_allocator());
    backlog->escape_backlog.drop(cz::heap_allocator());
    backlog->arena.drop();
}

void backlog_dec_refcount(cz::Slice<Backlog_State*> backlogs, Backlog_State* backlog) {
    CZ_DEBUG_ASSERT(backlog->refcount > 0);
    --backlog->refcount;
    if (backlog->refcount == 0)
        cleanup_backlog(backlogs, backlog);
}

char Backlog_State::get(size_t i) {
    CZ_DEBUG_ASSERT(i < length);
    return buffers[OUTER_INDEX(i)][INNER_INDEX(i)];
}

///////////////////////////////////////////////////////////////////////////////
// Module Code - append chunk
///////////////////////////////////////////////////////////////////////////////

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
        char* buffer = (char*)cz::heap_allocator().alloc({BACKLOG_BUFFER_SIZE, 1});
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

///////////////////////////////////////////////////////////////////////////////
// Module Code - Escape sequences - Utility
///////////////////////////////////////////////////////////////////////////////

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

static void set_graphics_rendition(Backlog_State* backlog, uint64_t graphics_rendition) {
    Backlog_Event event = {};
    event.index = backlog->length;
    event.type = BACKLOG_EVENT_SET_GRAPHIC_RENDITION;
    event.payload = graphics_rendition;
    backlog->events.reserve(cz::heap_allocator(), 1);
    backlog->events.push(event);
    backlog->graphics_rendition = graphics_rendition;
}

///////////////////////////////////////////////////////////////////////////////
// Module Code - Escape sequences - Parsing complicated ones
///////////////////////////////////////////////////////////////////////////////

static bool parse_extended_color(uint64_t* color, cz::Slice<int32_t> args, size_t* i) {
    if (*i + 2 >= args.len) {
        *i = args.len - 1;
        return false;
    }

    if (args[*i + 1] == 5) {
        if (args[*i + 2] == -1) {
            *i += 2;
            return false;
        }
        *color = args[*i + 2];
        *i += 2;
        return true;
    } else if (args[*i + 1] == 2) {
        // r = i + 2
        // g = i + 3
        // b = i + 4
        *i += 4;
    }
    return false;
}

static uint64_t parse_graphics_rendition(cz::Slice<int32_t> args, uint64_t graphics_rendition) {
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
        } else if ((args[i] >= 30 && args[i] <= 39) || (args[i] >= 90 && args[i] <= 99)) {
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
                if (!parse_extended_color(&color, args, &i))
                    color = 7;
            }
            graphics_rendition |= (color << GR_FOREGROUND_SHIFT);
        } else if ((args[i] >= 40 && args[i] <= 49) || (args[i] >= 100 && args[i] <= 109)) {
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
                if (!parse_extended_color(&color, args, &i))
                    color = 0;
            }
            graphics_rendition |= (color << GR_BACKGROUND_SHIFT);
        } else {
            // Ignored.
        }
    }

    return graphics_rendition;
}

static bool parse_hyperlink(Backlog_State* backlog, cz::Str fresh, size_t* skip) {
    // ESC]8;; <HYPERLINK> \a <TEXT> ESC]8;;
    // <TEXT> can have escape sequences in it and thus we preempt at the \a.

    cz::String* text = &backlog->escape_backlog;
    if (!ensure_char(backlog, 3, fresh, skip))
        return false;
    if (!ensure_char(backlog, 4, fresh, skip))
        return false;
    if ((*text)[3] != ';' || (*text)[4] != ';')
        CZ_PANIC("todo");

    Backlog_Event event = {};
    event.index = backlog->length;

    if (backlog->inside_hyperlink) {
        event.type = BACKLOG_EVENT_END_HYPERLINK;
    } else {
        size_t it = 5;
        while (1) {
            if (!ensure_char(backlog, it, fresh, skip))
                return false;
            char ch = (*text)[it];
            if (ch == '\a')
                break;
            ++it;
        }

        cz::Str url = text->slice(5, text->len - 1);
        event.payload = (uint64_t)url.clone_null_terminate(backlog->arena.allocator()).buffer;
        event.type = BACKLOG_EVENT_START_HYPERLINK;
    }

    backlog->inside_hyperlink = !backlog->inside_hyperlink;
    backlog->events.reserve(cz::heap_allocator(), 1);
    backlog->events.push(event);
    return true;
}

static bool parse_set_window_title(Backlog_State* backlog, cz::Str fresh, size_t* skip) {
    // ESC]0; <TITLE> BEL
    cz::String* text = &backlog->escape_backlog;
    if (!ensure_char(backlog, 3, fresh, skip))
        return false;
    if ((*text)[3] != ';')
        CZ_PANIC("todo");

    size_t it = 4;
    while (1) {
        if (!ensure_char(backlog, it, fresh, skip))
            return false;
        char ch = (*text)[it];
        if (ch == '\a') {
            // Alarm is the end of this sequence.
            text->remove(it);
            break;
        }
        ++it;
    }

    // Ignore the event.
    return true;
}

///////////////////////////////////////////////////////////////////////////////
// Module Code - Escape sequences - Wrapper and simple ones
///////////////////////////////////////////////////////////////////////////////

/// Attempt to process an escape sequence.  Returns `true` if it
/// was processed, `false` if we need more input to process it.
static bool process_escape_sequence(Backlog_State* backlog, cz::Str fresh, size_t* skip) {
    // Note(chris.gregory): At least right now we only care
    // about color escape sequences.  The rest we will discard.

    cz::String* text = &backlog->escape_backlog;

    if (!ensure_char(backlog, 0, fresh, skip))
        return false;

    if ((*text)[0] == '\r') {
        while (1) {
            if (!ensure_char(backlog, 1, fresh, skip))
                return false;

            // Ignore consecutive '\r's.
            if ((*text)[1] == '\r') {
                text->pop();
                continue;
            }

            if ((*text)[1] == '\n') {
                // '\r\n' -> '\n'
                append_chunk(backlog, "\n");
            } else {
                // '\rX' -> '\r' then process 'X'
                // TODO: this isn't right -- this should only move the cursor
                // and not change the line.  But in practice this works.
                uint64_t line_start = backlog->lines.len > 0 ? backlog->lines.last() : 0;
                truncate_to(backlog, line_start);
                --*skip;
            }
            return true;
        }
    }

    CZ_DEBUG_ASSERT((*text)[0] == 0x1b);

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
        if (!ensure_char(backlog, 2, fresh, skip))
            return false;

        if ((*text)[2] == '?') {
            size_t it = 3;

            // Parse code.
            int32_t arg = -1;
            if (!eat_number(backlog, &it, fresh, skip, &arg))
                return false;

            // Parse high or low flag.
            if (!ensure_char(backlog, it, fresh, skip))
                return false;
            bool high = false;
            if ((*text)[it] == 'h') {
                high = true;
            } else if ((*text)[it] == 'l') {
                high = false;
            } else {
                append_chunk(backlog, text->slice_start(1));
                return true;
            }

            if (arg == 12) {
                // Start/Stop Blinking
                (void)high;
            } else if (arg == 25) {
                // Show/Hide Cursor
            } else if (arg == 1) {
                // Enable/Disable Numlock
            } else if (arg == 3) {
                // Set Columns to 132/80
            } else if (arg == 1049) {
                // Enable/Disable Alternate Screen Buffer
            } else {
                append_chunk(backlog, text->slice_start(1));
            }
            return true;
        } else if ((*text)[2] == '!') {
            if (!ensure_char(backlog, 3, fresh, skip))
                return false;

            if ((*text)[3] == 'p') {
                uint64_t graphics_rendition = (7 << GR_FOREGROUND_SHIFT);
                set_graphics_rendition(backlog, graphics_rendition);
            } else {
                // Undo skipping the unrecognized character.
                CZ_DEBUG_ASSERT(*skip > 0);
                --*skip;
                append_chunk(backlog, "[!");
            }
            return true;
        } else {
            size_t it = 2;
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
            // ESC [ <n> ; <b> r    Set Scrolling Region between n and b (row numbers).
            if ((*text)[it] == 's' || (*text)[it] == 'u' || (*text)[it] == 'A' ||
                (*text)[it] == 'B' || (*text)[it] == 'D' || (*text)[it] == 'E' ||
                (*text)[it] == 'F' || (*text)[it] == 'G' || (*text)[it] == 'd' ||
                (*text)[it] == 'l' || (*text)[it] == 'Z' || (*text)[it] == 'S' ||
                (*text)[it] == 'T' || (*text)[it] == '@' || (*text)[it] == 'P' ||
                (*text)[it] == 'X' || (*text)[it] == 'L' || (*text)[it] == 'M' ||
                (*text)[it] == 'J' || (*text)[it] == 'K' || (*text)[it] == 'g' ||
                (*text)[it] == 'r') {
                return true;
            }

            // Ignoring these messages (this is probably going to cause bugs).
            // ESC [ 6 n            Report Cursor Position (term should print ESC [ <y> ; <x> R)
            // ESC [ 0 c            Report Device Attributes (term should print ESC [ ? 1 ; 0 c)
            else if ((*text)[it] == 'n' || (*text)[it] == 'c') {
                CZ_PANIC("todo");
                return true;
            }

            // ESC [ <ns> m         Set Graphic Rendition (series of commands to change
            //                      how future characters are rendered)
            else if ((*text)[it] == 'm') {
                uint64_t graphics_rendition = backlog->graphics_rendition;
                graphics_rendition = parse_graphics_rendition(args, graphics_rendition);
                set_graphics_rendition(backlog, graphics_rendition);
                return true;
            }

            // ESC [ <y> ; <x> H    Cursor Set Position
            // ESC [ <y> ; <x> f    Cursor Set Position
            else if ((*text)[it] == 'H' || (*text)[it] == 'f') {
                // Windows sends ESC [ H instead of CR so handle that.
                if (args.len > 0) {
                    // CZ_PANIC("todo");
                    return true;
                }
                uint64_t line_start = backlog->lines.len > 0 ? backlog->lines.last() : 0;
                truncate_to(backlog, line_start);
                return true;
            }

            // ESC [ <n> C          Cursor Forward
            else if ((*text)[it] == 'C') {
                // Instead of writing 12 spaces, conhost emits:
                // ESC [ 12 X ESC [ 96 m ESC [ 12 C
                // In other words, clear 12 characters, reset the rendition,
                // then move forward 12 characters.  We just ignore the clear
                // operation and count the "move forward" as inserting spaces.
                if (args.len >= 1) {
                    for (int32_t i = 0; i < args[0]; ++i) {
                        append_chunk(backlog, " ");
                    }
                }
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
    } else if ((*text)[1] == ']') {
        if (!ensure_char(backlog, 2, fresh, skip))
            return false;

        if ((*text)[2] == '8') {
            return parse_hyperlink(backlog, fresh, skip);
        } else if ((*text)[2] == '0') {
            return parse_set_window_title(backlog, fresh, skip);
        } else {
            CZ_PANIC("todo");
        }
    } else {
#if 0
        // The other escape sequences are key values so just pass them through with the escape.
        append_chunk(backlog, "\x1b");
#endif
        return true;
    }
}

static void truncate_to(Backlog_State* backlog, uint64_t new_length) {
    size_t outer_before = OUTER_INDEX(backlog->length);
    backlog->length = new_length;
    for (size_t i = outer_before + 1; i-- > OUTER_INDEX(backlog->length) + 1;) {
        cz::heap_allocator().dealloc({backlog->buffers.last(), BACKLOG_BUFFER_SIZE});
        backlog->buffers.pop();
    }
}

///////////////////////////////////////////////////////////////////////////////
// Module Code - Escape sequences - Main loop
///////////////////////////////////////////////////////////////////////////////

uint64_t append_text(Backlog_State* backlog, cz::Str text) {
    const char escape = 0x1b;
    const char del = 0x08;
    uint64_t done = 0;

    // If we are inside an escape sequence then pump the text into that first.
    if (backlog->escape_backlog.len != 0) {
        uint64_t skip = 0;
        if (!process_escape_sequence(backlog, text, &skip)) {
            // All of the text was consumed.
            return text.len;
        }

        backlog->escape_backlog.len = 0;
        text = text.slice_start(skip);
        done += skip;
    }

    while (text.len > 0) {
        // Find the first special character.
        size_t chunk_len = text.len;
        chunk_len = text.slice_end(chunk_len).find_index('\r');
        chunk_len = text.slice_end(chunk_len).find_index(escape);
        chunk_len = text.slice_end(chunk_len).find_index(del);
        chunk_len = text.slice_end(chunk_len).find_index('\a');

        // Append the normal text before it.
        uint64_t result = append_chunk(backlog, text.slice_end(chunk_len));
        done += result;

        // Output is truncated so just stop here.
        if (result != chunk_len)
            break;

        // No special character so stop.
        if (chunk_len == text.len)
            break;

        // Handle the special character.
        switch (text[chunk_len]) {
        case del: {
            // TODO: this isn't right -- this should only move the cursor
            // and not change the line.  But in practice this works.
            uint64_t line_start = backlog->lines.len > 0 ? backlog->lines.last() : 0;
            if (line_start < backlog->length)
                truncate_to(backlog, backlog->length - 1);
            text = text.slice_start(chunk_len + 1);
            done++;
        } break;

            // We want to handle '\r\r\n' by ignoring the '\r's so we
            // need to pull out the big guns: escape sequence parsing.
        case '\r':
        case escape: {
            // Start processing an escape sequence.
            cz::Str remaining = text.slice_start(chunk_len);
            uint64_t skip = 0;
            if (!process_escape_sequence(backlog, remaining, &skip)) {
                return done + remaining.len;
            }

            backlog->escape_backlog.len = 0;
            text = remaining.slice_start(skip);
            done += skip;
        } break;

        case '\a': {
            // Ignore alarm characters.
            text = text.slice_start(chunk_len + 1);
            done++;
        } break;
        }
    }

    return done;
}
