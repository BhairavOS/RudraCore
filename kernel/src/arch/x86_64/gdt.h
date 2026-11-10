#pragma once

#include "types.h"

namespace Rudracore::Arch::Gdt {

// Segment selectors. The user data/code order (data before code) is what the
// SYSRET instruction expects, which userspace will need in step 3.
constexpr u16 KERNEL_CODE = 0x08;
constexpr u16 KERNEL_DATA = 0x10;
constexpr u16 USER_DATA = 0x18 | 3;
constexpr u16 USER_CODE = 0x20 | 3;
constexpr u16 TSS = 0x28;

// Interrupt Stack Table slot used for double faults, so a kernel stack
// overflow still reaches the panic screen instead of triple-faulting.
constexpr u8 IST_DOUBLE_FAULT = 1;

void init();

// Stack the CPU switches to when an interrupt arrives from user mode.
void setKernelStack(u64 top);

} // namespace Rudracore::Arch::Gdt
