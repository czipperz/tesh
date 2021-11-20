#include <czt/test_base.hpp>

#include "shell.hpp"

TEST_CASE("parse_script empty line") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    CHECK(pipeline.pipeline.len == 0);
}

TEST_CASE("parse_script one word") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "abc");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 1);
    CHECK(pipeline.pipeline[0].args[0] == "abc");
}

TEST_CASE("parse_script two words") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "abc def");
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
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "   abc   def   ");
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
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "a | b");
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
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "a b|c d");
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
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "a '' 'b' 'abcabc'");
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
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "'");
    REQUIRE(error == Error_Parse);
    Parse_Pipeline pipeline = script.first.pipeline;
    CHECK(pipeline.pipeline.len == 0);
}

TEST_CASE("parse_script single quote unterminated 2") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "c  'b  \n  a");
    REQUIRE(error == Error_Parse);
    Parse_Pipeline pipeline = script.first.pipeline;
    CHECK(pipeline.pipeline.len == 0);
}

TEST_CASE("parse_script single quotes weird cases") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "' \n\n ' 'c'a'b'");
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
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "\"a\" \"\" \"abc\"");
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
        parse_script(&shell, cz::heap_allocator(), &script, {}, "\"\\\\ \\n \\a \\$ \\` \\\"\"");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 1);
    CHECK(pipeline.pipeline[0].args[0] == "\\ \\n \\a $ ` \"");
}

TEST_CASE("parse_script variable") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "a=b c=d");
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
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "a=b arg c=d");
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
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "$var$var");
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
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "\"$var$var\"");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 1);
    CHECK(pipeline.pipeline[0].args[0] == "$value$value");
}

TEST_CASE("parse_script 'echo $hi' hi is undefined") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "echo $hi");
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
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "\"$var\" echo $var");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 4);
    CHECK(pipeline.pipeline[0].args[0] == "a b");
    CHECK(pipeline.pipeline[0].args[1] == "echo");
    CHECK(pipeline.pipeline[0].args[2] == "a");
    CHECK(pipeline.pipeline[0].args[3] == "b");
}

TEST_CASE("parse_script backslash escapes dollar sign") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "\\$var");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 1);
    CHECK(pipeline.pipeline[0].args[0] == "$var");
}

TEST_CASE("parse_script dollar sign space") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "$ a");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == "$");
    CHECK(pipeline.pipeline[0].args[1] == "a");
}

TEST_CASE("parse_script dollar sign space in quotes") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "\"$ a\"");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 1);
    CHECK(pipeline.pipeline[0].args[0] == "$ a");
}

TEST_CASE("parse_script multi word variable a=$var keeps one word") {
    Shell_State shell = {};
    set_var(&shell, "var", "multi word");
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "a=$var");
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
                               {}, "echo < in arg1 > out arg2 2> err arg3");
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
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "echo 2 > out");
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
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "echo hi; echo bye");
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
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "echo hi \n echo bye");
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
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "echo hi && echo bye");
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
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "echo hi || echo bye");
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
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "echo hi & echo bye");
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
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "abc d\\\nef");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == "abc");
    CHECK(pipeline.pipeline[0].args[1] == "def");
}

TEST_CASE("parse_script backslash escapes newline inside string") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "abc \"d\\\nef\"");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == "abc");
    CHECK(pipeline.pipeline[0].args[1] == "def");
}

TEST_CASE("parse_script tilde not expanded") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "\\~ \"~\" \"~/\"");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 3);
    CHECK(pipeline.pipeline[0].args[0] == "~");
    CHECK(pipeline.pipeline[0].args[1] == "~");
    CHECK(pipeline.pipeline[0].args[2] == "~/");
}

TEST_CASE("parse_script tilde not expanded after start of word") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "a~/ $abc~/");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == "a~/");
    CHECK(pipeline.pipeline[0].args[1] == "~/");
}

TEST_CASE("parse_script tilde expanded simple") {
    Shell_State shell = {};
    set_var(&shell, "HOME", "/path/to/my/home");
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "~ ~/ ~/abc/123");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 3);
    CHECK(pipeline.pipeline[0].args[0] == "/path/to/my/home");
    CHECK(pipeline.pipeline[0].args[1] == "/path/to/my/home/");
    CHECK(pipeline.pipeline[0].args[2] == "/path/to/my/home/abc/123");
}

TEST_CASE("parse_script outer continuation 1") {
    Shell_State shell = {};
    Parse_Continuation outer = {};
    outer.success = cz::heap_allocator().alloc<Parse_Line>();
    outer.failure = cz::heap_allocator().alloc<Parse_Line>();
    outer.start = cz::heap_allocator().alloc<Parse_Line>();
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, outer, "a; b");
    REQUIRE(error == Error_Success);

    const Parse_Line* first = &script.first;
    CHECK(first->on.success == first->on.failure);
    CHECK(first->on.start == outer.start);
    REQUIRE(first->on.success);
    const Parse_Line* second = first->on.success;
    CHECK(second->on.success == outer.success);
    CHECK(second->on.failure == outer.failure);
    CHECK_FALSE(second->on.start);
}

TEST_CASE("parse_script outer continuation 2") {
    Shell_State shell = {};
    Parse_Continuation outer = {};
    outer.success = cz::heap_allocator().alloc<Parse_Line>();
    outer.failure = cz::heap_allocator().alloc<Parse_Line>();
    outer.start = cz::heap_allocator().alloc<Parse_Line>();
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, outer, "a && b");
    REQUIRE(error == Error_Success);

    const Parse_Line* first = &script.first;
    CHECK(first->on.failure == outer.failure);
    CHECK(first->on.start == outer.start);
    REQUIRE(first->on.success);
    const Parse_Line* second = first->on.success;
    CHECK(second->on.success == outer.success);
    CHECK(second->on.failure == outer.failure);
    CHECK_FALSE(second->on.start);
}

TEST_CASE("parse_script comment basic") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "# hi");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 0);
}

TEST_CASE("parse_script # after word isn't comment") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "a# hi");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == "a#");
    CHECK(pipeline.pipeline[0].args[1] == "hi");
}

TEST_CASE("parse_script # after empty string isn't comment") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "''# \"\"#");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == "#");
    CHECK(pipeline.pipeline[0].args[1] == "#");
}

TEST_CASE("parse_script # doesn't honor '\\\\\\n'") {
    Shell_State shell = {};
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "a #\\\nb");
    REQUIRE(error == Error_Success);

    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 1);
    CHECK(pipeline.pipeline[0].args[0] == "a");

    CHECK(script.first.on.failure == script.first.on.success);
    REQUIRE(script.first.on.success);

    pipeline = script.first.on.success->pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 1);
    CHECK(pipeline.pipeline[0].args[0] == "b");
}

TEST_CASE("parse_script alias simple") {
    Shell_State shell = {};
    set_alias(&shell, "aa", "bb");
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "aa cc");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == "bb");
    CHECK(pipeline.pipeline[0].args[1] == "cc");
}

TEST_CASE("parse_script alias not expanded when value of variable") {
    Shell_State shell = {};
    set_alias(&shell, "aa", "bb");
    set_var(&shell, "x", "aa");
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "$x cc");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == "aa");
    CHECK(pipeline.pipeline[0].args[1] == "cc");
}

TEST_CASE("parse_script alias not expanded when quoted") {
    Shell_State shell = {};
    set_alias(&shell, "aa", "bb");
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "'aa' cc");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == "aa");
    CHECK(pipeline.pipeline[0].args[1] == "cc");
}

TEST_CASE("parse_script expand ${x}") {
    Shell_State shell = {};
    set_var(&shell, "aa", "bb");
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "${aa} cc");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == "bb");
    CHECK(pipeline.pipeline[0].args[1] == "cc");
}

TEST_CASE("parse_script evaluate var expand before var set") {
    Shell_State shell = {};
    set_var(&shell, "aa", "cc");
    Parse_Script script = {};
    Error error = parse_script(&shell, cz::heap_allocator(), &script, {}, "aa=bb x${aa}y");
    REQUIRE(error == Error_Success);
    Parse_Pipeline pipeline = script.first.pipeline;
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 1);
    CHECK(pipeline.pipeline[0].args[0] == "xccy");
}
