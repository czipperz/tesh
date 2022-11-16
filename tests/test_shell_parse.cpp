#include <czt/test_base.hpp>

#include "global.hpp"
#include "shell.hpp"

extern uint64_t tesh_sub_counter;

static void test_append_node(cz::Allocator allocator,
                             cz::String* string,
                             Shell_Node* node,
                             size_t depth) {
    static const size_t spd = 4;
    cz::Str async_str = (node->async ? " (async)" : "");
    switch (node->type) {
    case Shell_Node::PROGRAM: {
        Parse_Program* program = node->v.program;
        append(allocator, string, cz::many(' ', depth * spd), "program", async_str, ":\n");
        for (size_t i = 0; i < program->subexprs.len; ++i) {
            append(allocator, string, cz::many(' ', (depth + 1) * spd), "sub", i, ":\n");
            test_append_node(allocator, string, program->subexprs[i], depth + 2);
        }
        for (size_t i = 0; i < program->variable_names.len; ++i) {
            append(allocator, string, cz::many(' ', (depth + 1) * spd), "var", i, ": ",
                   program->variable_names[i], '\n');
            append(allocator, string, cz::many(' ', (depth + 1) * spd), "val", i, ": ",
                   program->variable_values[i], '\n');
        }
        if (program->is_sub) {
            append(allocator, string, cz::many(' ', (depth + 1) * spd), "sub:\n");
            test_append_node(allocator, string, program->v.sub, depth + 2);
        } else {
            for (size_t i = 0; i < program->v.args.len; ++i) {
                append(allocator, string, cz::many(' ', (depth + 1) * spd), "arg", i, ": ",
                       program->v.args[i], '\n');
            }
        }
        if (program->in_file != "__tesh_std_in") {
            append(allocator, string, cz::many(' ', (depth + 1) * spd),
                   "in_file: ", program->in_file, '\n');
        }
        if (program->out_file != "__tesh_std_out") {
            append(allocator, string, cz::many(' ', (depth + 1) * spd),
                   "out_file: ", program->out_file, '\n');
        }
        if (program->err_file != "__tesh_std_err") {
            append(allocator, string, cz::many(' ', (depth + 1) * spd),
                   "err_file: ", program->err_file, '\n');
        }
    } break;

    case Shell_Node::PIPELINE: {
        append(allocator, string, cz::many(' ', depth * spd), "pipeline", async_str, ":\n");
        for (size_t i = 0; i < node->v.pipeline.len; ++i) {
            test_append_node(allocator, string, &node->v.pipeline[i], depth + 1);
        }
    } break;

    case Shell_Node::AND:
    case Shell_Node::OR: {
        cz::Str op = (node->type == Shell_Node::AND ? "and" : "or");
        append(allocator, string, cz::many(' ', depth * spd), op, async_str, ":\n");
        test_append_node(allocator, string, node->v.binary.left, depth + 1);
        test_append_node(allocator, string, node->v.binary.right, depth + 1);
    } break;

    case Shell_Node::SEQUENCE: {
        if (node->async || depth > 0) {
            append(allocator, string, cz::many(' ', depth * spd),
                   (node->async ? "async:\n" : "sync:\n"));
            ++depth;
        }
        for (size_t i = 0; i < node->v.sequence.len; ++i) {
            test_append_node(allocator, string, &node->v.sequence[i], depth);
        }
    } break;

    case Shell_Node::IF: {
        append(allocator, string, cz::many(' ', depth * spd), "if", async_str, ":\n");
        append(allocator, string, cz::many(' ', (depth + 1) * spd), "cond:\n");
        test_append_node(allocator, string, node->v.if_.cond, depth + 2);
        append(allocator, string, cz::many(' ', (depth + 1) * spd), "then:\n");
        test_append_node(allocator, string, node->v.if_.then, depth + 2);
        if (node->v.if_.other) {
            append(allocator, string, cz::many(' ', (depth + 1) * spd), "other:\n");
            test_append_node(allocator, string, node->v.if_.other, depth + 2);
        }
    } break;

    case Shell_Node::FUNCTION: {
        append(allocator, string, cz::many(' ', depth * spd), "function", async_str, ":\n");
        append(allocator, string, cz::many(' ', (depth + 1) * spd), "name: ", node->v.function.name,
               "\n");
        append(allocator, string, cz::many(' ', (depth + 1) * spd), "body:\n");
        test_append_node(allocator, string, node->v.function.body, depth + 2);
    } break;

    default:
        CZ_PANIC("Invalid Shell_Node type");
    }
}

static Error parse_and_emit(const Shell_State* shell, cz::String* string, cz::Str text) {
    Shell_Node root = {};
    Error error = parse_script(cz::heap_allocator(), &root, text);
    if (error == Error_Success)
        test_append_node(cz::heap_allocator(), string, &root, 0);
    return error;
}

static cz::Str expand(const Shell_State* shell, cz::Str arg) {
    cz::Vector<cz::Str> vec = {};
    expand_arg_split(&shell->local, arg, cz::heap_allocator(), &vec);

    cz::String string = {};
    for (size_t i = 0; i < vec.len; ++i) {
        cz::append(cz::heap_allocator(), &string, "arg", i, ": ", vec[i], '\n');
    }
    return string;
}

TEST_CASE("parse_script empty line") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() == "");
}

TEST_CASE("parse_script one word") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "abc");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
program:\n\
    arg0: abc\n");
}

TEST_CASE("parse_script two words") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "abc def");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
program:\n\
    arg0: abc\n\
    arg1: def\n");
}

TEST_CASE("parse_script two words whitespace") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "   abc   def   ");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
program:\n\
    arg0: abc\n\
    arg1: def\n");
}

TEST_CASE("parse_script pipe simple 1") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "a | b");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
pipeline:\n\
    program:\n\
        arg0: a\n\
    program:\n\
        arg0: b\n");
}

TEST_CASE("parse_script pipe simple 2") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "a b|c d");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
pipeline:\n\
    program:\n\
        arg0: a\n\
        arg1: b\n\
    program:\n\
        arg0: c\n\
        arg1: d\n");
}

TEST_CASE("parse_script single quotes basic cases") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "a '' 'b' 'abcabc'");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
program:\n\
    arg0: a\n\
    arg1: ''\n\
    arg2: 'b'\n\
    arg3: 'abcabc'\n");
}

TEST_CASE("parse_script single quote unterminated 1") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "'");
    REQUIRE(error == Error_Parse_UnterminatedString);
}

TEST_CASE("parse_script single quote unterminated 2") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "c  'b  \n  a");
    REQUIRE(error == Error_Parse_UnterminatedString);
}

TEST_CASE("parse_script single quotes weird cases") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "' \n\n ' 'c'a'b'");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
program:\n\
    arg0: ' \n\n '\n\
    arg1: 'c'a'b'\n");
}

TEST_CASE("parse_script double quote basic") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "\"a\" \"\" \"abc\"");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
program:\n\
    arg0: \"a\"\n\
    arg1: \"\"\n\
    arg2: \"abc\"\n");
}

TEST_CASE("parse_script double quote escape") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "\"\\\\ \\n \\a \\$ \\` \\\" \\& \\:\"");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
program:\n\
    arg0: \"\\\\ \\n \\a \\$ \\` \\\" \\& \\:\"\n");
}

TEST_CASE("parse_script double quote escape outside") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "\\\"ok");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
program:\n\
    arg0: \\\"ok\n");
}

TEST_CASE("parse_script variable ended in quote") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "\"$VARS\\\"\"");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
program:\n\
    arg0: \"$VARS\\\"\"\n");
}

TEST_CASE("parse_script variable") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "a=b c=d");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
program:\n\
    var0: a\n\
    val0: b\n\
    var1: c\n\
    val1: d\n");
}

TEST_CASE("parse_script variable after arg is arg") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "a=b arg c=d");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
program:\n\
    var0: a\n\
    val0: b\n\
    arg0: arg\n\
    arg1: c=d\n");
}

TEST_CASE("parse_script file indirection") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "echo < in arg1 > out arg2 2> err arg3");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
program:\n\
    arg0: echo\n\
    arg1: arg1\n\
    arg2: arg2\n\
    arg3: arg3\n\
    in_file: in\n\
    out_file: out\n\
    err_file: err\n");
}

TEST_CASE("parse_script file indirection stderr must be 2> no space") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "echo 2 > out");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
program:\n\
    arg0: echo\n\
    arg1: 2\n\
    out_file: out\n");
}

TEST_CASE("parse_script file indirection 2>&1") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "echo 2>&1");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
program:\n\
    arg0: echo\n\
    err_file: __tesh_std_out\n");
}

TEST_CASE("parse_script file indirection >&2") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "echo >&2");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
program:\n\
    arg0: echo\n\
    out_file: __tesh_std_err\n");
}

TEST_CASE("parse_script file indirection 2>&1 propagate") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "echo >file 2>&1");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
program:\n\
    arg0: echo\n\
    out_file: file\n\
    err_file: file\n");
}

TEST_CASE("parse_script file indirection >&2 propagate") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "echo 2>file >&2");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
program:\n\
    arg0: echo\n\
    out_file: file\n\
    err_file: file\n");
}

TEST_CASE("parse_script variable expand simple") {
    Shell_State shell = {};
    set_var(&shell.local, "var", "$value");
    REQUIRE(expand(&shell, "$var$var") == "arg0: $value$value\n");
}

TEST_CASE("parse_script variable expand inside quotes") {
    Shell_State shell = {};
    set_var(&shell.local, "var", "$value");
    REQUIRE(expand(&shell, "\"$var$var\"") == "arg0: $value$value\n");
}

TEST_CASE("parse_script 'echo $hi' hi is undefined") {
    Shell_State shell = {};
    REQUIRE(expand(&shell, "$hi") == "");
}

TEST_CASE("parse_script multi word variable expanded") {
    Shell_State shell = {};
    set_var(&shell.local, "var", "a b");
    REQUIRE(expand(&shell, "$var") == "arg0: a\narg1: b\n");
}

TEST_CASE("parse_script backslash escapes dollar sign") {
    Shell_State shell = {};
    REQUIRE(expand(&shell, "\\$var\\&\\:") == "arg0: $var&:\n");
}

TEST_CASE("parse_script backslash escapes double quote") {
    Shell_State shell = {};
    REQUIRE(expand(&shell, "\\\"") == "arg0: \"\n");
}

TEST_CASE("parse_script dollar sign space in quotes") {
    Shell_State shell = {};
    REQUIRE(expand(&shell, "\"$ a\"") == "arg0: $ a\n");
}

TEST_CASE("parse_script semicolon combiner") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "echo hi; echo bye");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
program:\n\
    arg0: echo\n\
    arg1: hi\n\
program:\n\
    arg0: echo\n\
    arg1: bye\n");
}

TEST_CASE("parse_script newline combiner") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "echo hi \n echo bye");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
program:\n\
    arg0: echo\n\
    arg1: hi\n\
program:\n\
    arg0: echo\n\
    arg1: bye\n");
}

TEST_CASE("parse_script && combiner") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "echo hi && echo bye");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
and:\n\
    program:\n\
        arg0: echo\n\
        arg1: hi\n\
    program:\n\
        arg0: echo\n\
        arg1: bye\n");
}

TEST_CASE("parse_script || combiner") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "echo hi || echo bye");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
or:\n\
    program:\n\
        arg0: echo\n\
        arg1: hi\n\
    program:\n\
        arg0: echo\n\
        arg1: bye\n");
}

TEST_CASE("parse_script && || precedence") {
    Shell_State shell = {};
    cz::String string = {};
    Error error =
        parse_and_emit(&shell, &string, "echo hi && echo bye || echo hello && echo world");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
or:\n\
    and:\n\
        program:\n\
            arg0: echo\n\
            arg1: hi\n\
        program:\n\
            arg0: echo\n\
            arg1: bye\n\
    and:\n\
        program:\n\
            arg0: echo\n\
            arg1: hello\n\
        program:\n\
            arg0: echo\n\
            arg1: world\n");
}

TEST_CASE("parse_script & combiner") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "echo hi & echo bye");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
program (async):\n\
    arg0: echo\n\
    arg1: hi\n\
program:\n\
    arg0: echo\n\
    arg1: bye\n");
}

TEST_CASE("parse_script backslash escapes newline") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "d\\\nef");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
program:\n\
    arg0: d\\\nef\n");

    REQUIRE(expand(&shell, "d\\\nef") == "arg0: def\n");
}

TEST_CASE("parse_script backslash escapes newline inside string") {
    Shell_State shell = {};
    REQUIRE(expand(&shell, "\"d\\\nef\"") == "arg0: def\n");
}

TEST_CASE("parse_script tilde not expanded") {
    Shell_State shell = {};
    CHECK(expand(&shell, "\\~") == "arg0: ~\n");
    CHECK(expand(&shell, "\"~\"") == "arg0: ~\n");
    CHECK(expand(&shell, "\"~/\"") == "arg0: ~/\n");
}

TEST_CASE("parse_script tilde not expanded after start of word") {
    Shell_State shell = {};
    CHECK(expand(&shell, "a~/") == "arg0: a~/\n");
    CHECK(expand(&shell, "$abc~/") == "arg0: ~/\n");
}

TEST_CASE("parse_script tilde expanded simple") {
    Shell_State shell = {};
    set_var(&shell.local, "HOME", "/path/to/my/home");
    CHECK(expand(&shell, "~") == "arg0: /path/to/my/home\n");
    CHECK(expand(&shell, "~/") == "arg0: /path/to/my/home/\n");
    CHECK(expand(&shell, "~/abc/123") == "arg0: /path/to/my/home/abc/123\n");
}

TEST_CASE("parse_script tilde expanded variable assignment") {
    Shell_State shell = {};
    set_var(&shell.local, "HOME", "/path/to/my/home");
    CHECK(expand(&shell, "x=~") == "arg0: x=/path/to/my/home\n");
    CHECK(expand(&shell, "x=~/") == "arg0: x=/path/to/my/home/\n");
    CHECK(expand(&shell, "x=:~") == "arg0: x=:/path/to/my/home\n");
    CHECK(expand(&shell, "x=\\\\:~") == "arg0: x=\\:/path/to/my/home\n");
    CHECK(expand(&shell, "x=a~") == "arg0: x=a~\n");
    CHECK(expand(&shell, "x=\\:~") == "arg0: x=:~\n");
    CHECK(expand(&shell, "x==~") == "arg0: x==~\n");
    CHECK(expand(&shell, "=~") == "arg0: =~\n");
    CHECK(expand(&shell, ":~") == "arg0: :~\n");
    CHECK(expand(&shell, "a:~") == "arg0: a:~\n");
}

TEST_CASE("parse_script argument expansion 1") {
    Shell_State shell = {};
    shell.local.args.reserve(cz::heap_allocator(), 3);
    shell.local.args.push("program");
    shell.local.args.push("thearg1");
    shell.local.args.push("thearg2");
    CHECK(expand(&shell, "\"$@\"") ==
          "\
arg0: thearg1\n\
arg1: thearg2\n");
}

TEST_CASE("parse_script argument expansion 2") {
    Shell_State shell = {};
    shell.local.args.reserve(cz::heap_allocator(), 3);
    shell.local.args.push("program");
    shell.local.args.push("thearg1 has spaces");
    shell.local.args.push("thearg2");
    CHECK(expand(&shell, "\"$@\"") ==
          "\
arg0: thearg1 has spaces\n\
arg1: thearg2\n");
}

TEST_CASE("parse_script argument expansion 3") {
    Shell_State shell = {};
    shell.local.args.reserve(cz::heap_allocator(), 3);
    shell.local.args.push("program");
    shell.local.args.push("thearg1 has spaces");
    shell.local.args.push("thearg2");
    CHECK(expand(&shell, "$@") ==
          "\
arg0: thearg1\n\
arg1: has\n\
arg2: spaces\n\
arg3: thearg2\n");
}

TEST_CASE("parse_script argument expansion 4") {
    Shell_State shell = {};
    shell.local.args.reserve(cz::heap_allocator(), 3);
    shell.local.args.push("program");
    shell.local.args.push("thearg1");
    shell.local.args.push("thearg2");
    CHECK(expand(&shell, "\"$*\"") == "arg0: thearg1 thearg2\n");
}

TEST_CASE("parse_script argument expansion 5") {
    temp_allocator = cz::heap_allocator();

    Shell_State shell = {};
    shell.local.args.reserve(cz::heap_allocator(), 3);
    shell.local.args.push("program");
    shell.local.args.push("thearg1 has spaces");
    shell.local.args.push("thearg2");
    CHECK(expand(&shell, "$#") == "arg0: 2\n");
}

TEST_CASE("parse_script comment basic") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "# hi");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() == "");
}

TEST_CASE("parse_script # after word isn't comment") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "a# hi");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
program:\n\
    arg0: a#\n\
    arg1: hi\n");
}

TEST_CASE("parse_script # after empty string isn't comment") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "''# \"\"#");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
program:\n\
    arg0: ''#\n\
    arg1: \"\"#\n");
}

TEST_CASE("parse_script # doesn't honor '\\\\\\n'") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "a #\\\nb");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
program:\n\
    arg0: a\n\
program:\n\
    arg0: b\n");
}

#if 0
TEST_CASE("parse_script alias simple") {
    Shell_State shell = {};
    set_alias(&shell, "aa", "bb");
    Parse_Script script = {};
    Error error = tokenize(&shell, cz::heap_allocator(), &script, "aa cc");
    REQUIRE(error == Error_Success);
    size_t index = 0;
    Parse_Pipeline pipeline;
    error = parse_pipeline(script.tokens, &index, cz::heap_allocator(), &pipeline);
    REQUIRE(error == Error_Success);
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == "bb");
    CHECK(pipeline.pipeline[0].args[1] == "cc");
}

TEST_CASE("parse_script alias not expanded when value of variable") {
    Shell_State shell = {};
    set_alias(&shell, "aa", "bb");
    set_var(&shell.local, "x", "aa");
    Parse_Script script = {};
    Error error = tokenize(&shell, cz::heap_allocator(), &script, "$x cc");
    REQUIRE(error == Error_Success);
    size_t index = 0;
    Parse_Pipeline pipeline;
    error = parse_pipeline(script.tokens, &index, cz::heap_allocator(), &pipeline);
    REQUIRE(error == Error_Success);
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == "aa");
    CHECK(pipeline.pipeline[0].args[1] == "cc");
}

TEST_CASE("parse_script alias not expanded when quoted") {
    Shell_State shell = {};
    set_alias(&shell, "aa", "bb");
    Parse_Script script = {};
    Error error = tokenize(&shell, cz::heap_allocator(), &script, "'aa' cc");
    REQUIRE(error == Error_Success);
    size_t index = 0;
    Parse_Pipeline pipeline;
    error = parse_pipeline(script.tokens, &index, cz::heap_allocator(), &pipeline);
    REQUIRE(error == Error_Success);
    REQUIRE(pipeline.pipeline.len == 1);
    REQUIRE(pipeline.pipeline[0].args.len == 2);
    CHECK(pipeline.pipeline[0].args[0] == "aa");
    CHECK(pipeline.pipeline[0].args[1] == "cc");
}
#endif

TEST_CASE("parse_script expand ${x}") {
    Shell_State shell = {};
    set_var(&shell.local, "aa", "bb");
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "${aa} cc");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
program:\n\
    arg0: ${aa}\n\
    arg1: cc\n");

    REQUIRE(expand(&shell, "${aa}") == "arg0: bb\n");
}

TEST_CASE("parse_script ()") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "(a ; b) && (c || d)");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
and:\n\
    program:\n\
        sub:\n\
            sync:\n\
                program:\n\
                    arg0: a\n\
                program:\n\
                    arg0: b\n\
    program:\n\
        sub:\n\
            or:\n\
                program:\n\
                    arg0: c\n\
                program:\n\
                    arg0: d\n");
}

TEST_CASE("parse_script if statement basic") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "if true; then echo; fi");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
if:\n\
    cond:\n\
        program:\n\
            arg0: true\n\
    then:\n\
        program:\n\
            arg0: echo\n");
}

TEST_CASE("parse_script if else statement") {
    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "if true; then echo hi; else echo bye; fi");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
if:\n\
    cond:\n\
        program:\n\
            arg0: true\n\
    then:\n\
        program:\n\
            arg0: echo\n\
            arg1: hi\n\
    other:\n\
        program:\n\
            arg0: echo\n\
            arg1: bye\n");
}

TEST_CASE("parse_script if elif statement") {
    Shell_State shell = {};
    cz::String string = {};
    Error error =
        parse_and_emit(&shell, &string, "if true; then echo hi; elif cond2; then echo bye; fi");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
if:\n\
    cond:\n\
        program:\n\
            arg0: true\n\
    then:\n\
        program:\n\
            arg0: echo\n\
            arg1: hi\n\
    other:\n\
        if:\n\
            cond:\n\
                program:\n\
                    arg0: cond2\n\
            then:\n\
                program:\n\
                    arg0: echo\n\
                    arg1: bye\n");
}

TEST_CASE("parse_script function basic") {
    // Temporary hack.
    permanent_allocator = cz::heap_allocator();

    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "f() { echo }; }");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
function:\n\
    name: f\n\
    body:\n\
        program:\n\
            arg0: echo\n\
            arg1: }\n");
}

TEST_CASE("parse_script subexpr $()") {
    // Temporary hack.
    permanent_allocator = cz::heap_allocator();
    tesh_sub_counter = 0;

    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "a$(c x$(y)z d)b");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
pipeline:\n\
    program:\n\
        sub:\n\
            sync:\n\
                pipeline:\n\
                    program:\n\
                        arg0: y\n\
                    program:\n\
                        arg0: __tesh_set_var\n\
                        arg1: __tesh_sub0\n\
                program:\n\
                    arg0: c\n\
                    arg1: x${__tesh_sub0}z\n\
                    arg2: d\n\
    program:\n\
        arg0: __tesh_set_var\n\
        arg1: __tesh_sub1\n\
program:\n\
    arg0: a${__tesh_sub1}b\n");
}

TEST_CASE("parse_script assign var $()") {
    // Temporary hack.
    permanent_allocator = cz::heap_allocator();
    tesh_sub_counter = 0;

    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "x=$(hi)");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
pipeline:\n\
    program:\n\
        arg0: hi\n\
    program:\n\
        arg0: __tesh_set_var\n\
        arg1: __tesh_sub0\n\
program:\n\
    var0: x\n\
    val0: ${__tesh_sub0}\n\
");
}

TEST_CASE("parse_script subexpr $() inside quotes") {
    // Temporary hack.
    permanent_allocator = cz::heap_allocator();
    tesh_sub_counter = 0;

    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "a\"$(c x$(y)z d)\"b");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
pipeline:\n\
    program:\n\
        sub:\n\
            sync:\n\
                pipeline:\n\
                    program:\n\
                        arg0: y\n\
                    program:\n\
                        arg0: __tesh_set_var\n\
                        arg1: __tesh_sub0\n\
                program:\n\
                    arg0: c\n\
                    arg1: x${__tesh_sub0}z\n\
                    arg2: d\n\
    program:\n\
        arg0: __tesh_set_var\n\
        arg1: __tesh_sub1\n\
program:\n\
    arg0: a\"${__tesh_sub1}\"b\n");
}

TEST_CASE("parse_script subexpr <()") {
    // Temporary hack.
    permanent_allocator = cz::heap_allocator();
    tesh_sub_counter = 0;

    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "a<(c <(y) d)b");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
pipeline:\n\
    program:\n\
        arg0: mktemp\n\
    program:\n\
        arg0: __tesh_set_var\n\
        arg1: __tesh_sub1\n\
program:\n\
    sub:\n\
        sync:\n\
            pipeline:\n\
                program:\n\
                    arg0: mktemp\n\
                program:\n\
                    arg0: __tesh_set_var\n\
                    arg1: __tesh_sub0\n\
            program:\n\
                sub:\n\
                    program:\n\
                        arg0: y\n\
                in_file: /dev/null\n\
                out_file: ${__tesh_sub0}\n\
            program:\n\
                arg0: c\n\
                arg1: ${__tesh_sub0}\n\
                arg2: d\n\
    in_file: /dev/null\n\
    out_file: ${__tesh_sub1}\n\
program:\n\
    arg0: a${__tesh_sub1}b\n");
}

TEST_CASE("parse_script subexpr <() inside quotes does nothing") {
    // Temporary hack.
    permanent_allocator = cz::heap_allocator();
    tesh_sub_counter = 0;

    Shell_State shell = {};
    cz::String string = {};
    Error error = parse_and_emit(&shell, &string, "a\"<(c x<(y)z d)\"b");
    REQUIRE(error == Error_Success);
    CHECK(string.as_str() ==
          "\
program:\n\
    arg0: a\"<(c x<(y)z d)\"b\n");
}
