#include <czt/test_base.hpp>

#include "shell.hpp"

TEST_CASE("parse_line empty line") {
    Shell_State shell = {};
    Parse_Line out = {};
    Error error = parse_line(&shell, cz::heap_allocator(), &out, "");
    CHECK(out.pipeline.len == 0);
    REQUIRE(error == Error_Success);
}

TEST_CASE("parse_line one word") {
    Shell_State shell = {};
    Parse_Line out = {};
    Error error = parse_line(&shell, cz::heap_allocator(), &out, "abc");
    REQUIRE(error == Error_Success);
    REQUIRE(out.pipeline.len == 1);
    REQUIRE(out.pipeline[0].args.len == 1);
    CHECK(out.pipeline[0].args[0] == "abc");
}

TEST_CASE("parse_line two words") {
    Shell_State shell = {};
    Parse_Line out = {};
    Error error = parse_line(&shell, cz::heap_allocator(), &out, "abc def");
    REQUIRE(error == Error_Success);
    REQUIRE(out.pipeline.len == 1);
    REQUIRE(out.pipeline[0].args.len == 2);
    CHECK(out.pipeline[0].args[0] == "abc");
    CHECK(out.pipeline[0].args[1] == "def");
}

TEST_CASE("parse_line two words whitespace") {
    Shell_State shell = {};
    Parse_Line out = {};
    Error error = parse_line(&shell, cz::heap_allocator(), &out, "   abc   def   ");
    REQUIRE(error == Error_Success);
    REQUIRE(out.pipeline.len == 1);
    REQUIRE(out.pipeline[0].args.len == 2);
    CHECK(out.pipeline[0].args[0] == "abc");
    CHECK(out.pipeline[0].args[1] == "def");
}

TEST_CASE("parse_line pipe simple 1") {
    Shell_State shell = {};
    Parse_Line out = {};
    Error error = parse_line(&shell, cz::heap_allocator(), &out, "a | b");
    REQUIRE(error == Error_Success);
    REQUIRE(out.pipeline.len == 2);
    REQUIRE(out.pipeline[0].args.len == 1);
    CHECK(out.pipeline[0].args[0] == "a");
    REQUIRE(out.pipeline[1].args.len == 1);
    CHECK(out.pipeline[1].args[0] == "b");
}

TEST_CASE("parse_line pipe simple 2") {
    Shell_State shell = {};
    Parse_Line out = {};
    Error error = parse_line(&shell, cz::heap_allocator(), &out, "a b|c d");
    REQUIRE(error == Error_Success);
    REQUIRE(out.pipeline.len == 2);
    REQUIRE(out.pipeline[0].args.len == 2);
    CHECK(out.pipeline[0].args[0] == "a");
    CHECK(out.pipeline[0].args[1] == "b");
    REQUIRE(out.pipeline[1].args.len == 2);
    CHECK(out.pipeline[1].args[0] == "c");
    CHECK(out.pipeline[1].args[1] == "d");
}

TEST_CASE("parse_line single quotes basic cases") {
    Shell_State shell = {};
    Parse_Line out = {};
    Error error = parse_line(&shell, cz::heap_allocator(), &out, "a '' 'b' 'abcabc'");
    REQUIRE(error == Error_Success);
    REQUIRE(out.pipeline.len == 1);
    REQUIRE(out.pipeline[0].args.len == 4);
    CHECK(out.pipeline[0].args[0] == "a");
    CHECK(out.pipeline[0].args[1] == "");
    CHECK(out.pipeline[0].args[2] == "b");
    CHECK(out.pipeline[0].args[3] == "abcabc");
}

TEST_CASE("parse_line single quote unterminated 1") {
    Shell_State shell = {};
    Parse_Line out = {};
    Error error = parse_line(&shell, cz::heap_allocator(), &out, "'");
    REQUIRE(error == Error_Parse);
    CHECK(out.pipeline.len == 0);
}

TEST_CASE("parse_line single quote unterminated 2") {
    Shell_State shell = {};
    Parse_Line out = {};
    Error error = parse_line(&shell, cz::heap_allocator(), &out, "c  'b  \n  a");
    REQUIRE(error == Error_Parse);
    CHECK(out.pipeline.len == 0);
}

TEST_CASE("parse_line single quotes weird cases") {
    Shell_State shell = {};
    Parse_Line out = {};
    Error error = parse_line(&shell, cz::heap_allocator(), &out, "' \n\n ' 'c'a'b'");
    REQUIRE(error == Error_Success);
    REQUIRE(out.pipeline.len == 1);
    REQUIRE(out.pipeline[0].args.len == 2);
    CHECK(out.pipeline[0].args[0] == " \n\n ");
    CHECK(out.pipeline[0].args[1] == "cab");
}

TEST_CASE("parse_line double quote basic") {
    Shell_State shell = {};
    Parse_Line out = {};
    Error error = parse_line(&shell, cz::heap_allocator(), &out, "\"a\" \"\" \"abc\"");
    REQUIRE(error == Error_Success);
    REQUIRE(out.pipeline.len == 1);
    REQUIRE(out.pipeline[0].args.len == 3);
    CHECK(out.pipeline[0].args[0] == "a");
    CHECK(out.pipeline[0].args[1] == "");
    CHECK(out.pipeline[0].args[2] == "abc");
}

TEST_CASE("parse_line double quote escape") {
    Shell_State shell = {};
    Parse_Line out = {};
    Error error = parse_line(&shell, cz::heap_allocator(), &out, "\"\\\\ \\n \\a \\$ \\` \\\"\"");
    REQUIRE(error == Error_Success);
    REQUIRE(out.pipeline.len == 1);
    REQUIRE(out.pipeline[0].args.len == 1);
    CHECK(out.pipeline[0].args[0] == "\\ \\n \\a $ ` \"");
}

TEST_CASE("parse_line variable") {
    Shell_State shell = {};
    Parse_Line out = {};
    Error error = parse_line(&shell, cz::heap_allocator(), &out, "a=b c=d");
    REQUIRE(error == Error_Success);
    REQUIRE(out.pipeline.len == 1);
    CHECK(out.pipeline[0].args.len == 0);
    REQUIRE(out.pipeline[0].variable_names.len == 2);
    REQUIRE(out.pipeline[0].variable_values.len == 2);
    CHECK(out.pipeline[0].variable_names[0] == "a");
    CHECK(out.pipeline[0].variable_values[0] == "b");
    CHECK(out.pipeline[0].variable_names[1] == "c");
    CHECK(out.pipeline[0].variable_values[1] == "d");
}

TEST_CASE("parse_line variable after arg is arg") {
    Shell_State shell = {};
    Parse_Line out = {};
    Error error = parse_line(&shell, cz::heap_allocator(), &out, "a=b arg c=d");
    REQUIRE(error == Error_Success);
    REQUIRE(out.pipeline.len == 1);
    REQUIRE(out.pipeline[0].args.len == 2);
    REQUIRE(out.pipeline[0].variable_names.len == 1);
    REQUIRE(out.pipeline[0].variable_values.len == 1);
    CHECK(out.pipeline[0].variable_names[0] == "a");
    CHECK(out.pipeline[0].variable_values[0] == "b");
    CHECK(out.pipeline[0].args[0] == "arg");
    CHECK(out.pipeline[0].args[1] == "c=d");
}

TEST_CASE("parse_line variable expand simple") {
    Shell_State shell = {};
    set_env_var(&shell, "var", "$value");
    Parse_Line out = {};
    Error error = parse_line(&shell, cz::heap_allocator(), &out, "$var$var");
    REQUIRE(error == Error_Success);
    REQUIRE(out.pipeline.len == 1);
    REQUIRE(out.pipeline[0].args.len == 1);
    CHECK(out.pipeline[0].args[0] == "$value$value");
}

TEST_CASE("parse_line variable expand inside quotes") {
    Shell_State shell = {};
    set_env_var(&shell, "var", "$value");
    Parse_Line out = {};
    Error error = parse_line(&shell, cz::heap_allocator(), &out, "\"$var$var\"");
    REQUIRE(error == Error_Success);
    REQUIRE(out.pipeline.len == 1);
    REQUIRE(out.pipeline[0].args.len == 1);
    CHECK(out.pipeline[0].args[0] == "$value$value");
}

TEST_CASE("parse_line 'echo $hi' hi is undefined") {
    Shell_State shell = {};
    Parse_Line out = {};
    Error error = parse_line(&shell, cz::heap_allocator(), &out, "echo $hi");
    REQUIRE(error == Error_Success);
    REQUIRE(out.pipeline.len == 1);
    REQUIRE(out.pipeline[0].args.len == 1);
    CHECK(out.pipeline[0].args[0] == "echo");
}

TEST_CASE("parse_line multi word variable expanded") {
    Shell_State shell = {};
    set_env_var(&shell, "var", "a b");
    Parse_Line out = {};
    Error error = parse_line(&shell, cz::heap_allocator(), &out, "\"$var\" echo $var");
    REQUIRE(error == Error_Success);
    REQUIRE(out.pipeline.len == 1);
    REQUIRE(out.pipeline[0].args.len == 4);
    CHECK(out.pipeline[0].args[0] == "a b");
    CHECK(out.pipeline[0].args[1] == "echo");
    CHECK(out.pipeline[0].args[2] == "a");
    CHECK(out.pipeline[0].args[3] == "b");
}

TEST_CASE("parse_line multi word variable a=$var keeps one word") {
    Shell_State shell = {};
    set_env_var(&shell, "var", "multi word");
    Parse_Line out = {};
    Error error = parse_line(&shell, cz::heap_allocator(), &out, "a=$var");
    REQUIRE(error == Error_Success);
    REQUIRE(out.pipeline.len == 1);
    CHECK(out.pipeline[0].args.len == 0);
    REQUIRE(out.pipeline[0].variable_names.len == 1);
    REQUIRE(out.pipeline[0].variable_values.len == 1);
    CHECK(out.pipeline[0].variable_names[0] == "a");
    CHECK(out.pipeline[0].variable_values[0] == "multi word");
}

TEST_CASE("parse_line file indirection") {
    Shell_State shell = {};
    Parse_Line out = {};
    Error error = parse_line(&shell, cz::heap_allocator(), &out, "echo < in arg1 > out arg2 2> err arg3");
    REQUIRE(error == Error_Success);
    REQUIRE(out.pipeline.len == 1);
    REQUIRE(out.pipeline[0].args.len == 4);
    CHECK(out.pipeline[0].args[0] == "echo");
    CHECK(out.pipeline[0].args[1] == "arg1");
    CHECK(out.pipeline[0].args[2] == "arg2");
    CHECK(out.pipeline[0].args[3] == "arg3");

    CHECK(out.pipeline[0].in_file == "in");
    CHECK(out.pipeline[0].out_file == "out");
    CHECK(out.pipeline[0].err_file == "err");
}

TEST_CASE("parse_line file indirection stderr must be 2> no space") {
    Shell_State shell = {};
    Parse_Line out = {};
    Error error = parse_line(&shell, cz::heap_allocator(), &out, "echo 2 > out");
    REQUIRE(error == Error_Success);
    REQUIRE(out.pipeline.len == 1);
    REQUIRE(out.pipeline[0].args.len == 2);
    CHECK(out.pipeline[0].args[0] == "echo");
    CHECK(out.pipeline[0].args[1] == "2");

    CHECK(out.pipeline[0].out_file == "out");
}
