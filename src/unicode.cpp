#include "unicode.hpp"

#include <string.h>
#include <cz/assert.hpp>

namespace unicode {

size_t utf8_width(uint8_t ch) {
    // 1 byte sequence: 0xxxxxxx
    if ((ch >> 7) == 0x0)
        return 1;

    // continuation character: 10xxxxxx
    if ((ch >> 6) == 0x2)
        return 1;

    // 2 byte sequence: 110xxxxx
    if ((ch >> 5) == 0x6)
        return 2;

    // 3 byte sequence: 1110xxxx
    if ((ch >> 4) == 0xe)
        return 3;

    // 4 byte sequence: 11110xxx
    if ((ch >> 3) == 0x1e)
        return 4;

    // others are undefined (return 1 to just skip the byte)
    return 1;
}

bool utf8_is_continuation(uint8_t ch) {
    // Continuation character is 10xxxxxx.
    return (ch >> 6) == 2;
}

uint32_t utf8_code_point(const uint8_t* seq) {
    size_t len = strlen((const char*)seq);
    switch (len) {
    case 1:
        // Either 1 byte sequence: 0xxxxxxx or garbage byte.
        return (uint32_t)seq[0];
    case 2:
        // 2 byte sequence: 110xxxxx 10xxxxxx
        return (((uint32_t)seq[0] & 0x1f) << 6) |  //
               (((uint32_t)seq[1] & 0x3f));
    case 3:
        // 3 byte sequence: 1110xxxx 10xxxxxx 10xxxxxx
        return (((uint32_t)seq[0] & 0x0f) << 12) |  //
               (((uint32_t)seq[1] & 0x3f) << 6) |   //
               (((uint32_t)seq[2] & 0x3f));
    case 4:
        // 4 byte sequence: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
        return (((uint32_t)seq[0] & 0x07) << 18) |  //
               (((uint32_t)seq[1] & 0x3f) << 12) |  //
               (((uint32_t)seq[2] & 0x3f) << 6) |   //
               (((uint32_t)seq[3] & 0x3f));
    default:
        CZ_PANIC("unreachable");
    }
}

}
