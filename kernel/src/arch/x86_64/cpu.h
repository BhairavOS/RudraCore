#pragma once

#include "types.h"

namespace Rudracore::Arch {

inline u64 readCr2() {
    u64 value;
    asm volatile("mov %%cr2, %0" : "=r"(value));
    return value;
}

inline u64 readCr3() {
    u64 value;
    asm volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

inline void writeCr3(u64 value) {
    asm volatile("mov %0, %%cr3" : : "r"(value) : "memory");
}

inline void invalidatePage(u64 virt) {
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

inline void disableInterrupts() {
    asm volatile("cli" ::: "memory");
}

inline u64 currentFramePointer() {
    return reinterpret_cast<u64>(__builtin_frame_address(0));
}

} // namespace Rudracore::Arch
