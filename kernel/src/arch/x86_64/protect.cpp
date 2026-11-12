#include "arch/x86_64/protect.h"

#include "log.h"
#include "panic.h"

namespace Rudracore::Arch::Protect {

namespace {

constexpr u32 MSR_EFER = 0xC0000080;
constexpr u64 EFER_NXE = 1ull << 11;
constexpr u64 CR0_WP = 1ull << 16;
constexpr u64 CR4_SMEP = 1ull << 20;
constexpr u64 CR4_SMAP = 1ull << 21;

void cpuid(u32 leaf, u32 sub, u32& a, u32& b, u32& c, u32& d) {
    asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(leaf), "c"(sub));
}

u64 readMsr(u32 msr) {
    u32 lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (u64{hi} << 32) | lo;
}

void writeMsr(u32 msr, u64 value) {
    asm volatile("wrmsr" : : "c"(msr), "a"(static_cast<u32>(value)), "d"(static_cast<u32>(value >> 32)));
}

u64 readCr0() {
    u64 v;
    asm volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}

u64 readCr4() {
    u64 v;
    asm volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

} // namespace

void init() {
    u32 a, b, c, d;
    cpuid(0x80000000, 0, a, b, c, d);
    bool hasNx = false;
    if (a >= 0x80000001) {
        cpuid(0x80000001, 0, a, b, c, d);
        hasNx = d & (1u << 20);
    }
    if (not hasNx)
        panic("cpu: no NX (execute-disable) support; RudraCore needs it");
    writeMsr(MSR_EFER, readMsr(MSR_EFER) | EFER_NXE);

    u64 cr0 = readCr0() | CR0_WP;
    asm volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

void enableSmepSmap() {
    u32 a, b, c, d;
    cpuid(0, 0, a, b, c, d);
    bool smep = false, smap = false;
    if (a >= 7) {
        cpuid(7, 0, a, b, c, d);
        smep = b & (1u << 7);
        smap = b & (1u << 20);
    }

    u64 cr4 = readCr4() | (smep ? CR4_SMEP : 0) | (smap ? CR4_SMAP : 0);
    asm volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
    smapEnabled = smap;
    if (smap)
        asm volatile("clac" ::: "memory");

    log("[cpu]  nx on, write protect on, smep ", smep ? "on" : "off (not supported)",
        ", smap ", smap ? "on" : "off (not supported)");
}

} // namespace Rudracore::Arch::Protect
