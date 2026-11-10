#pragma once

#include "types.h"

// Floating point and SSE for user programs.
//
// The kernel itself is built without SSE, so it never touches these
// registers; the scheduler just saves one task's state and loads the next
// one's on every switch (FXSAVE/FXRSTOR, 512 bytes per task).
namespace Rudracore::Arch::Fpu {

constexpr usize STATE_SIZE = 512;

// Turns on x87 and SSE (CR0/CR4) and resets them.
void init();

// A clean state: all exceptions masked, round to nearest (what a fresh
// program expects).
void initState(u8* state);

inline void save(u8* state) {
    asm volatile("fxsave64 (%0)" : : "r"(state) : "memory");
}

inline void restore(u8 const* state) {
    asm volatile("fxrstor64 (%0)" : : "r"(state) : "memory");
}

} // namespace Rudracore::Arch::Fpu
