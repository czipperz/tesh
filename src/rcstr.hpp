#pragma once

#include <cz/heap.hpp>
#include <cz/string.hpp>

struct RcStr {
    uint64_t* rc;
    cz::Str str;

    static RcStr create_clone(cz::Str arg) {
        RcStr rcstr;
        rcstr.rc = cz::heap_allocator().alloc<uint64_t>();
        CZ_ASSERT(rcstr.rc);
        *rcstr.rc = 1;
        rcstr.str = arg.clone_null_terminate(cz::heap_allocator());
        return rcstr;
    }

    inline void increment() {
        ++*rc;
    }

    inline RcStr dup() {
        ++*rc;
        return *this;
    }

    void drop() {
        --*rc;
        if (*rc == 0) {
            cz::heap_allocator().dealloc({(char*)str.buffer, str.len + 1});
        }
    }
};
