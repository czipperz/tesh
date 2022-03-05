#include "shell.hpp"

static const Builtin level0[] = {
    {"exit", Running_Program::EXIT},
    {"return", Running_Program::RETURN},
    {"cd", Running_Program::CD},
    {"alias", Running_Program::ALIAS},
    {"function", Running_Program::FUNCTION},
    {"export", Running_Program::EXPORT},
    {"unset", Running_Program::UNSET},
    {"clear", Running_Program::CLEAR},
    {".", Running_Program::SOURCE},
    {"source", Running_Program::SOURCE},
    {"sleep", Running_Program::SLEEP},
    {"configure", Running_Program::CONFIGURE},
    {"attach", Running_Program::ATTACH},
    {"follow", Running_Program::FOLLOW},
    {"argdump", Running_Program::ARGDUMP},
    {"vardump", Running_Program::VARDUMP},
    {"shift", Running_Program::SHIFT},
    {"history", Running_Program::HISTORY},
    {"__tesh_set_var", Running_Program::SET_VAR},
    {"builtin", Running_Program::BUILTIN},
};

static const Builtin level1[] = {
    {"echo", Running_Program::ECHO},    {"pwd", Running_Program::PWD},
    {"which", Running_Program::WHICH},  {"true", Running_Program::TRUE_},
    {"false", Running_Program::FALSE_},
};

static const Builtin level2[] = {
    {"cat", Running_Program::CAT},
    {"ls", Running_Program::LS},
};

static const cz::Slice<const Builtin> the_builtin_levels[] = {level0, level1, level2};

const cz::Slice<const cz::Slice<const Builtin> > builtin_levels = the_builtin_levels;
