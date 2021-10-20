#include <czt/test_base.hpp>

#include "shell_parse.hpp"

TEST_CASE("parse_line empty line") {
    Shell_State shell = {};
    Shell_Line out = {};
    Error error = parse_line(&shell, cz::heap_allocator(), &out, "");
    CHECK(out.pipeline.len == 0);
    REQUIRE(error == ERROR_SUCCESS);
}

TEST_CASE("parse_line one word") {
    Shell_State shell = {};
    Shell_Line out = {};
    Error error = parse_line(&shell, cz::heap_allocator(), &out, "abc");
    REQUIRE(error == ERROR_SUCCESS);
    REQUIRE(out.pipeline.len == 1);
    REQUIRE(out.pipeline[0].words.len == 1);
    CHECK(out.pipeline[0].words[0] == "abc");
}

TEST_CASE("parse_line two words") {
    Shell_State shell = {};
    Shell_Line out = {};
    Error error = parse_line(&shell, cz::heap_allocator(), &out, "abc def");
    REQUIRE(error == ERROR_SUCCESS);
    REQUIRE(out.pipeline.len == 1);
    REQUIRE(out.pipeline[0].words.len == 2);
    CHECK(out.pipeline[0].words[0] == "abc");
    CHECK(out.pipeline[0].words[1] == "def");
}

TEST_CASE("parse_line two words whitespace") {
    Shell_State shell = {};
    Shell_Line out = {};
    Error error = parse_line(&shell, cz::heap_allocator(), &out, "   abc   def   ");
    REQUIRE(error == ERROR_SUCCESS);
    REQUIRE(out.pipeline.len == 1);
    REQUIRE(out.pipeline[0].words.len == 2);
    CHECK(out.pipeline[0].words[0] == "abc");
    CHECK(out.pipeline[0].words[1] == "def");
}
