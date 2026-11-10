#pragma once

#include "types.h"

// Kernel heap: small objects come from per-size free lists carved out of
// whole pages; anything bigger than 2 KiB gets its own pages from the PMM.
// Every block has a 16-byte header so free() knows where it came from and
// can catch bad or double frees.
namespace Rudracore::Heap {

void* alloc(usize size);

void free(void* ptr);

struct Stats {
    u64 bytesInUse; // requested sizes of live allocations
    u64 pagesUsed;  // pages taken from the PMM
};

Stats stats();

} // namespace Rudracore::Heap
