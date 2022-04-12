#include "error.hpp"

#include <cz/assert.hpp>

const char* error_string(Error error) {
    switch (error) {
    case Error_Success:
        return "Success";
    case Error_IO:
        return "I/O error";
    case Error_Parse_UnterminatedString:
        return "Unterminated string while parsing";
    case Error_Parse_UnterminatedVariable:
        return "Unterminated variable while parsing";
    case Error_Parse_UnterminatedProgram:
        return "Unterminated program while parsing";
    case Error_Parse_UnterminatedParen:
        return "Unterminated parenthesis expression while parsing";
    case Error_Parse_UnterminatedIf:
        return "Unterminated if statement while parsing";
    case Error_Parse_UnterminatedFunctionDeclaration:
        return "Unterminated function declaration while parsing";
    case Error_Parse_UnterminatedSubExpr:
        return "Unterminated sub expr";
    case Error_Parse_StrayCloseParen:
        return "Stray close paren";
    case Error_Parse_ExpectedEndOfStatement:
        return "Expected end of statement, found a token instead";
    case Error_Parse_EmptyProgram:
        return "Unexpected empty program";
    case Error_Parse_NothingToIndirect:
        return "Nothing to indirect";
    case Error_InvalidPath:
        return "Invalid path";
    case Error_InvalidProgram:
        return "Invalid program";
    default:
        CZ_PANIC("Invalid Error value");
    }
}
