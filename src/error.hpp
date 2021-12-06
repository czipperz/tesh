#pragma once

enum Error {
    Error_Success,
    Error_IO,
    Error_Parse_UnterminatedString,
    Error_Parse_UnterminatedVariable,
    Error_Parse_EmptyProgram,
    Error_Parse_NothingToIndirect,
    Error_InvalidPath,
    Error_InvalidProgram,
};
