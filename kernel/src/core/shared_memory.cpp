#include "core/shared_memory.h"

#include "mem/memory.h"
#include "mem/pmm.h"

namespace Rudracore::Core {

SharedMemory::~SharedMemory() {
    if (owned)
        Pmm::free(physical, size / Mem::PAGE_SIZE);
    if (charged) {
        charged->refund(size / Mem::PAGE_SIZE);
        charged->unref();
    }
}

SharedMemory* SharedMemory::allocate(u64 size) {
    if (size == 0 or size > MAX_SIZE)
        return nullptr;

    u64 aligned = Mem::alignUp(size, Mem::PAGE_SIZE);
    if (not Pmm::userCanUse(aligned / Mem::PAGE_SIZE + 1)) // + the object itself
        return nullptr;
    u64 physical = Pmm::allocZeroed(aligned / Mem::PAGE_SIZE);
    if (not physical)
        return nullptr;

    auto* memory = new SharedMemory();
    memory->physical = physical;
    memory->size = aligned;
    memory->owned = true;
    return memory;
}

SharedMemory* SharedMemory::device(u64 physical, u64 size, Vmm::Cache cache) {
    auto* memory = new SharedMemory();
    memory->physical = Mem::alignDown(physical, Mem::PAGE_SIZE);
    memory->size = Mem::alignUp(physical + size, Mem::PAGE_SIZE) - memory->physical;
    memory->owned = false;
    memory->cache = cache;
    return memory;
}

} // namespace Rudracore::Core
