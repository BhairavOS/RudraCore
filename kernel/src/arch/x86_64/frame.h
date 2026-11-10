#pragma once

#include "types.h"

namespace Rudracore::Arch {

// CPU state saved on interrupt entry, in stack order (lowest address first).
// Must match the push order in interrupts.S.
struct [[gnu::packed]] Frame {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;

    u64 vector;
    u64 error; // CPU error code, or 0 for vectors that don't push one

    // Pushed by the CPU.
    u64 rip, cs, rflags, rsp, ss;
};

static_assert(sizeof(Frame) == 22 * 8);

} // namespace Rudracore::Arch
