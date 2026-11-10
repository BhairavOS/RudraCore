#include "arch/x86_64/fpu.h"

extern "C" void* memset(void* dest, int value, usize n);

namespace Rudracore::Arch::Fpu {

namespace {

constexpr u64 CR0_MP = 1 << 1; // monitor coprocessor
constexpr u64 CR0_EM = 1 << 2; // emulation: must be off
constexpr u64 CR0_TS = 1 << 3; // task switched: off, we switch eagerly
constexpr u64 CR4_OSFXSR = 1 << 9;      // FXSAVE/FXRSTOR and SSE
constexpr u64 CR4_OSXMMEXCPT = 1 << 10; // SSE exceptions raise #XM

constexpr u16 DEFAULT_FCW = 0x037F;   // x87: all exceptions masked, 64-bit precision
constexpr u32 DEFAULT_MXCSR = 0x1F80; // SSE: all exceptions masked, round to nearest

} // namespace

void init() {
    u64 cr0, cr4;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr0 = (cr0 & ~(CR0_EM | CR0_TS)) | CR0_MP;
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    asm volatile("mov %0, %%cr0" : : "r"(cr0));
    asm volatile("mov %0, %%cr4" : : "r"(cr4));

    u32 mxcsr = DEFAULT_MXCSR;
    asm volatile("fninit; ldmxcsr %0" : : "m"(mxcsr));
}

void initState(u8* state) {
    memset(state, 0, STATE_SIZE);
    *reinterpret_cast<u16*>(state + 0) = DEFAULT_FCW;
    *reinterpret_cast<u32*>(state + 24) = DEFAULT_MXCSR;
}

} // namespace Rudracore::Arch::Fpu
