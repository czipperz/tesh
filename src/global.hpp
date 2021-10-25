#pragma once

#include <cz/allocator.hpp>
#include <cz/str.hpp>

extern cz::Allocator temp_allocator;
extern cz::Allocator permanent_allocator;

extern cz::Str program_name;
extern cz::Str program_directory;

void set_program_name(cz::Str fallback);
void set_program_directory();
