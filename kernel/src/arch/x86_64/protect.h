#pragma once

#include "types.h"

// The CPU's memory protections the kernel relies on.
namespace Rudracore::Arch::Protect {

// NX (EFER.NXE) and CR0.WP: ring 0 can't write read-only pages either.
// Panics if the CPU has no NX: page tables use the NX bit everywhere.
// Call before the kernel's own page tables are loaded.
void init();

// SMEP (ring 0 never runs user pages) and SMAP (ring 0 never touches user
// pages, except inside a UserAccess). Each only if the CPU has it. Call once
// the kernel runs on its own page tables.
void enableSmepSmap();

inline bool smapEnabled = false;

// Lets the kernel touch user memory for the guard's lifetime (STAC/CLAC).
// Only around copies from or to user memory the caller has checked.
struct UserAccess {
    UserAccess() {
        if (smapEnabled)
            asm volatile("stac" ::: "memory");
    }
    ~UserAccess() {
        if (smapEnabled)
            asm volatile("clac" ::: "memory");
    }
    UserAccess(UserAccess const&) = delete;
    UserAccess& operator=(UserAccess const&) = delete;
};

// User mode can set RFLAGS.AC itself, and an interrupt doesn't clear it:
// run on every kernel entry so SMAP can't be switched off from ring 3.
inline void closeUserAccess() {
    if (smapEnabled)
        asm volatile("clac" ::: "memory");
}

} // namespace Rudracore::Arch::Protect
