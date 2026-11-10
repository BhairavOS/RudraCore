#pragma once

// Kapalaframe: the program heap, behind `new` and `delete`.
//
// Small blocks (up to 2 KiB) come from per-size free lists; bigger ones get
// their own pages and are kept for reuse when freed. Memory comes from the
// kernel in chunks (SYS_MEMORY_ALLOC) and goes back when the program exits.

namespace Kapala::Heap {

void* alloc(unsigned long size);
void free(void* pointer);

struct Stats {
    unsigned long bytesInUse;
    unsigned long bytesFromKernel;
};

Stats stats();

} // namespace Kapala::Heap
