#pragma once

#include "boot.h"

// Physical memory manager: hands out 4 KiB physical pages.
//
// A bitmap with one bit per page (1 = used) covers all RAM up to the highest
// usable address. The bitmap itself is stored in the first usable region big
// enough to hold it. Not thread-safe yet: the kernel is single-core with
// interrupts off until step 3.
namespace Rudracore::Pmm {

struct Stats {
    u64 managedPages; // pages ever given to the allocator
    u64 freePages;
    u64 bitmapBytes;
};

void init(Boot::Info const& info);

// Allocates `count` physically contiguous pages. Returns the physical address
// of the first one, or 0 if there's not enough memory (page 0 is never handed
// out, so 0 always means failure).
u64 alloc(usize count = 1);

// Same as alloc(), with the pages filled with zeros.
u64 allocZeroed(usize count = 1);

void free(u64 physical, usize count = 1);

// Gives the bootloader's memory (its page tables, stack and boot info) to the
// allocator. Only safe once the kernel runs on its own page tables and stack
// and has copied everything it needs out of the boot info.
void reclaimBootloaderMemory(Boot::Info const& info);

Stats stats();

} // namespace Rudracore::Pmm
