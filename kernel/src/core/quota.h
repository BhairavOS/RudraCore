#pragma once

#include "types.h"

namespace Rudracore::Core {

// How much memory one program may hold, in pages: the private memory it
// allocates and the shared memory it creates (counted until that is freed,
// even after it was handed to another program). So no single program can
// take all of RAM from the others; the kernel's own reserve is in Pmm.
//
// Reference counted: held by its task and by every object charged to it,
// so a refund after the task has ended still has somewhere to go.
// Only touched with interrupts disabled (syscalls).
struct Quota {
    u64 limit;
    u64 used = 0;
    u32 refs = 1;

    explicit Quota(u64 limitPages) : limit(limitPages) {}

    bool charge(u64 pages) {
        if (pages > limit - used)
            return false;
        used += pages;
        return true;
    }

    void refund(u64 pages) { used -= pages; }

    void ref() { refs++; }

    void unref() {
        if (--refs == 0)
            delete this;
    }
};

} // namespace Rudracore::Core
