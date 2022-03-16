#pragma once

struct Search_State {
    bool is_searching;

    Prompt_State prompt;

    uint64_t outer, inner;
};
