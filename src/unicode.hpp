#pragma once

#include <stddef.h>
#include <stdint.h>

namespace unicode {

size_t utf8_width(uint8_t ch);
bool utf8_is_continuation(uint8_t ch);
uint32_t utf8_code_point(const uint8_t* seq);

}
