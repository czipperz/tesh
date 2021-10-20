#pragma once

#include "shell.hpp"
#include "shell_parse.hpp"

///////////////////////////////////////////////////////////////////////////////

Error start_execute_line(Shell_State* shell, const Parse_Line& line, uint64_t id);
