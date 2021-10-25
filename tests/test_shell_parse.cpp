#include <czt/test_base.hpp>

#include "shell.hpp"

TEST_CASE("parse_script empty line") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, "");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    CHECK(pipeline.pipeline.len == 0);
}

TEST_CASE("parse_script one word") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, "abc");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 1);
    CHECK(pipeline.pipeline[0].args[0] == "abc");
}

TEST_CASE("parse_script two words") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, "abc def");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == "abc");
    CHECK(pipeline.pipeline[0].args[1] == "def");
}

TEST_CASE("parse_script two words whitespace") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, "   abc   def   ");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == "abc");
    CHECK(pipeline.pipeline[0].args[1] == "def");
}

TEST_CASE("parse_script pipe simple 1") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, "a | b");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 2);
    REQUIRE(pipeline.pipeline[0].args.len == 1);
    CHECK(pipeline.pipeline[0].args[0] == "a");
    REQUIRE(pipeline.pipeline[1].args.len == 1);
    CHECK(pipeline.pipeline[1].args[0] == "b");
}

TEST_CASE("parse_script pipe simple 2") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, "a b|c d");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 2);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == "a");
    CHECK(pipeline.pipeline[0].args[1] == "b");
    REQUIRE(pipeline.pipeline[1].args.len == 2);
    CHECK(pipeline.pipeline[1].args[0] == "c");
    CHECK(pipeline.pipeline[1].args[1] == "d");
}

TEST_CASE("parse_script single quotes basic cases") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, "a '' 'b' 'abcabc'");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 4);
    CHECK(pipeline.pipeline[0].args[0] == "a");
    CHECK(pipeline.pipeline[0].args[1] == "");
    CHECK(pipeline.pipeline[0].args[2] == "b");
    CHECK(pipeline.pipeline[0].args[3] == "abcabc");
}

TEST_CASE("parse_script single quote unterminated 1") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, "'");
    REQUIRE(error == Error_Parse);
    Parse_Pipeline pipeline = script.first.pipeline;
    CHECK(pipeline.pipeline.len == 0);
}

TEST_CASE("parse_script single quote unterminated 2") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, "c  'b  \n  a");
    REQUIRE(error == Error_Parse);
    Parse_Pipeline pipeline = script.first.pipeline;
    CHECK(pipeline.pipeline.len == 0);
}

TEST_CASE("parse_script single quotes weird cases") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, "' \n\n ' 'c'a'b'");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == " \n\n ");
    CHECK(pipeline.pipeline[0].args[1] == "cab");
}

TEST_CASE("parse_script double quote basic") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, "\"a\" \"\" \"abc\"");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 3);
    CHECK(pipeline.pipeline[0].args[0] == "a");
    CHECK(pipeline.pipeline[0].args[1] == "");
    CHECK(pipeline.pipeline[0].args[2] == "abc");
}

TEST_CASE("parse_script double quote escape") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error =
        parse_script(&shell, cz::heap_allocator(), &script, "\"\\\\ \\n \\a \\$ \\` \\\"\"");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 1);
    CHECK(pipeline.pipeline[0].args[0] == "\\ \\n \\a $ ` \"");
}

TEST_CASE("parse_script variable") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, "a=b c=d");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    CHECK(pipeline.pipeline[0].args.len == 0);
    REQUIRE(pipeline.pipeline[0].variable_names.len == 2);
    REQUIRE(pipeline.pipeline[0].variable_values.len == 2);
    CHECK(pipeline.pipeline[0].variable_names[0] == "a");
    CHECK(pipeline.pipeline[0].variable_values[0] == "b");
    CHECK(pipeline.pipeline[0].variable_names[1] == "c");
    CHECK(pipeline.pipeline[0].variable_values[1] == "d");
}

TEST_CASE("parse_script variable after arg is arg") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, "a=b arg c=d");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    REQUIRE(pipeline.pipeline[0].variable_names.len == 1);
    REQUIRE(pipeline.pipeline[0].variable_values.len == 1);
    CHECK(pipeline.pipeline[0].variable_names[0] == "a");
    CHECK(pipeline.pipeline[0].variable_values[0] == "b");
    CHECK(pipeline.pipeline[0].args[0] == "arg");
    CHECK(pipeline.pipeline[0].args[1] == "c=d");
}

TEST_CASE("parse_script variable expand simple") {
    Shell_State shell = {};
    set_var(&shell, "var", "$value");
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, "$var$var");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 1);
    CHECK(pipeline.pipeline[0].args[0] == "$value$value");
}

TEST_CASE("parse_script variable expand inside quotes") {
    Shell_State shell = {};
    set_var(&shell, "var", "$value");
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, "\"$var$var\"");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 1);
    CHECK(pipeline.pipeline[0].args[0] == "$value$value");
}

TEST_CASE("parse_script 'echo $hi' hi is undefined") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, "echo $hi");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 1);
    CHECK(pipeline.pipeline[0].args[0] == "echo");
}

TEST_CASE("parse_script multi word variable expanded") {
    Shell_State shell = {};
    set_var(&shell, "var", "a b");
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, "\"$var\" echo $var");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 4);
    CHECK(pipeline.pipeline[0].args[0] == "a b");
    CHECK(pipeline.pipeline[0].args[1] == "echo");
    CHECK(pipeline.pipeline[0].args[2] == "a");
    CHECK(pipeline.pipeline[0].args[3] == "b");
}

TEST_CASE("parse_script multi word variable a=$var keeps one word") {
    Shell_State shell = {};
    set_var(&shell, "var", "multi word");
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, "a=$var");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    CHECK(pipeline.pipeline[0].args.len == 0);
    REQUIRE(pipeline.pipeline[0].variable_names.len == 1);
    REQUIRE(pipeline.pipeline[0].variable_values.len == 1);
    CHECK(pipeline.pipeline[0].variable_names[0] == "a");
    CHECK(pipeline.pipeline[0].variable_values[0] == "multi word");
}

TEST_CASE("parse_script file indirection") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script,
                               "echo < in arg1 > out arg2 2> err arg3");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 4);
    CHECK(pipeline.pipeline[0].args[0] == "echo");
    CHECK(pipeline.pipeline[0].args[1] == "arg1");
    CHECK(pipeline.pipeline[0].args[2] == "arg2");
    CHECK(pipeline.pipeline[0].args[3] == "arg3");

    CHECK(pipeline.pipeline[0].in_file == "in");
    CHECK(pipeline.pipeline[0].out_file == "out");
    CHECK(pipeline.pipeline[0].err_file == "err");
}

TEST_CASE("parse_script file indirection stderr must be 2> no space") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, "echo 2 > out");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == "echo");
    CHECK(pipeline.pipeline[0].args[1] == "2");

    CHECK(pipeline.pipeline[0].out_file == "out");
}

TEST_CASE("parse_script semicolon combiner") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, "echo hi; echo bye");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == "echo");
    CHECK(pipeline.pipeline[0].args[1] == "hi");

    REQUIRE(script.first.on.success);
    CHECK(script.first.on.success == script.first.on.failure);
    CHECK_FALSE(script.first.on.success->on.success);

    pipeline = script.first.on.success->pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == "echo");
    CHECK(pipeline.pipeline[0].args[1] == "bye");
}

TEST_CASE("parse_script newline combiner") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, "echo hi \n echo bye");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == "echo");
    CHECK(pipeline.pipeline[0].args[1] == "hi");

    REQUIRE(script.first.on.success);
    CHECK(script.first.on.success == script.first.on.failure);
    CHECK_FALSE(script.first.on.success->on.success);

    pipeline = script.first.on.success->pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == "echo");
    CHECK(pipeline.pipeline[0].args[1] == "bye");
}

TEST_CASE("parse_script && combiner") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, "echo hi && echo bye");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == "echo");
    CHECK(pipeline.pipeline[0].args[1] == "hi");

    REQUIRE(script.first.on.success);
    CHECK_FALSE(script.first.on.failure);
    CHECK_FALSE(script.first.on.success->on.success);

    pipeline = script.first.on.success->pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == "echo");
    CHECK(pipeline.pipeline[0].args[1] == "bye");
}

TEST_CASE("parse_script || combiner") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, "echo hi || echo bye");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == "echo");
    CHECK(pipeline.pipeline[0].args[1] == "hi");

    REQUIRE(script.first.on.failure);
    CHECK_FALSE(script.first.on.success);
    CHECK_FALSE(script.first.on.failure->on.success);

    pipeline = script.first.on.failure->pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == "echo");
    CHECK(pipeline.pipeline[0].args[1] == "bye");
}

TEST_CASE("parse_script & combiner") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, "echo hi & echo bye");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == "echo");
    CHECK(pipeline.pipeline[0].args[1] == "hi");

    REQUIRE(script.first.on.start);
    CHECK_FALSE(script.first.on.success);
    CHECK_FALSE(script.first.on.failure);
    CHECK_FALSE(script.first.on.start->on.success);

    pipeline = script.first.on.start->pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == "echo");
    CHECK(pipeline.pipeline[0].args[1] == "bye");
}

TEST_CASE("parse_script backslash escapes newline") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, "abc d\\\nef");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == "abc");
    CHECK(pipeline.pipeline[0].args[1] == "def");
}
