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

inline void enableInterrupts() {
    asm volatile("sti" ::: "memory");
}

// Sleeps until the next interrupt.
inline void waitForInterrupt() {
    asm volatile("hlt" ::: "memory");
}

// Disables interrupts for the lifetime of the guard, then restores the
// previous state. On a single CPU this is the kernel's lock: nothing can
// preempt code holding a guard. Guards can be nested.
struct InterruptGuard {
    u64 flags;

    InterruptGuard() {
        asm volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    }

    ~InterruptGuard() {
        if (flags & (1 << 9)) // IF was set
            asm volatile("sti" ::: "memory");
    }

    InterruptGuard(InterruptGuard const&) = delete;
    InterruptGuard& operator=(InterruptGuard const&) = delete;
};

inline u64 currentFramePointer() {
    return reinterpret_cast<u64>(__builtin_frame_address(0));
}

} // namespace Rudracore::Arch
