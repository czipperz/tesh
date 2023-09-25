#include <czt/test_base.hpp>

#include <cz/defer.hpp>
#include <cz/file.hpp>
#include "backlog.hpp"

#define BBS BACKLOG_BUFFER_SIZE

TEST_CASE("backlog append_text handle huge write") {
    cz::String data = {};
    CZ_DEFER(data.drop(cz::heap_allocator()));
    REQUIRE(cz::read_to_string("src/backlog.cpp", cz::heap_allocator(), &data));
    REQUIRE(data.len >= 3 * BBS);

    Backlog_State backlog = {};
    init_backlog(&backlog, /*id=*/0, /*max_length=*/1ull << 30 /*1GB*/);

    // Fill first buffer.
    append_text(&backlog, data.slice(0ull, BBS / 8));
    append_text(&backlog, data.slice(BBS / 8, BBS / 2));
    append_text(&backlog, data.slice(BBS / 2, BBS));

    // Fill second buffer and part of third.
    append_text(&backlog, data.slice(BBS, BBS + BBS / 8));
    append_text(&backlog, data.slice(BBS + BBS / 8, BBS * 2 + BBS / 16));
    append_text(&backlog, data.slice(BBS * 2 + BBS / 16, BBS * 2 + BBS / 8));

    cz::String output = dbg_stringify_backlog(&backlog);
    CZ_DEFER(output.drop(cz::heap_allocator()));
    CHECK(output == data.slice_end(BBS * 2 + BBS / 8));
}
