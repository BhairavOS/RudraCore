#pragma once

#include "types.h"

namespace Rudracore::Core {

// What one program may hold: pages of memory (the private memory it
// allocates and the shared memory it creates) and kernel objects (channels,
// shared memory). Shared memory and objects count until they are freed,
// even after being handed to another program. So no single program can
// take all of RAM or fill the kernel heap; the kernel's own reserve is in Pmm.
//
// Reference counted: held by its task and by every object charged to it,
// so a refund after the task has ended still has somewhere to go.
// Only touched with interrupts disabled (syscalls).
struct Quota {
    static constexpr u64 OBJECT_LIMIT = 1024;     // system services
    static constexpr u64 APP_OBJECT_LIMIT = 256;  // apps

    u64 limit;
    u64 used = 0;
    u64 objects = 0;
    u64 objectLimit = OBJECT_LIMIT;
    u32 refs = 1;

    explicit Quota(u64 limitPages) : limit(limitPages) {}

    bool charge(u64 pages) {
        if (pages > limit - used)
            return false;
        used += pages;
        return true;
    }

    void refund(u64 pages) { used -= pages; }

    bool chargeObjects(u64 n) {
        if (n > objectLimit - objects)
            return false;
        objects += n;
        return true;
    }

    void refundObjects(u64 n) { objects -= n; }

    void ref() { refs++; }

    void unref() {
        if (--refs == 0)
            delete this;
    }
};

} // namespace Rudracore::Core
