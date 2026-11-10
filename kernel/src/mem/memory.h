#pragma once

#include "types.h"

namespace Rudracore::Mem {

constexpr u64 PAGE_SIZE = 4096;

constexpr u64 alignDown(u64 value, u64 align) { return value & ~(align - 1); }
constexpr u64 alignUp(u64 value, u64 align) { return alignDown(value + align - 1, align); }

// All physical memory is reachable at physical + hhdmOffset ("higher half
// direct map"), set from the boot info before any memory manager runs.
inline u64 hhdmOffset = 0;

template <typename T = void>
T* toVirtual(u64 physical) {
    return reinterpret_cast<T*>(physical + hhdmOffset);
}

} // namespace Rudracore::Mem
