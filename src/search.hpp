#pragma once

struct Search_State {
    bool is_searching;
    bool default_forwards;

    Prompt_State prompt;

    uint64_t outer, inner;
};
