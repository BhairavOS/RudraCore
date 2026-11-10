#include "arch/x86_64/gdt.h"

namespace Rudracore::Arch::Gdt {

namespace {

struct [[gnu::packed]] Tss {
    u32 reserved0;
    u64 rsp[3];
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    u16 reserved3;
    u16 ioMapBase;
};
static_assert(sizeof(Tss) == 104);

struct [[gnu::packed]] Pointer {
    u16 limit;
    u64 base;
};

// Flat 64-bit segments: only the access byte and the long-mode bit matter.
constexpr u64 KERNEL_CODE_DESC = 0x00af9a000000ffff;
constexpr u64 KERNEL_DATA_DESC = 0x00cf92000000ffff;
constexpr u64 USER_DATA_DESC = 0x00cff2000000ffff;
constexpr u64 USER_CODE_DESC = 0x00affa000000ffff;

alignas(16) u64 entries[7];
alignas(16) Tss tss;
alignas(16) u8 doubleFaultStack[16 * 1024];
alignas(16) u8 interruptStack[16 * 1024];

// A TSS descriptor takes two GDT slots in long mode.
void setTssDescriptor(usize slot, u64 base, u32 limit) {
    entries[slot] = (limit & 0xFFFF) |
                    ((base & 0xFFFFFF) << 16) |
                    (0x89ull << 40) | // present, 64-bit available TSS
                    (u64((limit >> 16) & 0xF) << 48) |
                    (((base >> 24) & 0xFF) << 56);
    entries[slot + 1] = base >> 32;
}

} // namespace

void init() {
    entries[0] = 0;
    entries[KERNEL_CODE / 8] = KERNEL_CODE_DESC;
    entries[KERNEL_DATA / 8] = KERNEL_DATA_DESC;
    entries[USER_DATA / 8] = USER_DATA_DESC;
    entries[USER_CODE / 8] = USER_CODE_DESC;

    tss = {};
    tss.rsp[0] = reinterpret_cast<u64>(interruptStack + sizeof(interruptStack));
    tss.ist[IST_DOUBLE_FAULT - 1] = reinterpret_cast<u64>(doubleFaultStack + sizeof(doubleFaultStack));
    tss.ioMapBase = sizeof(Tss); // no I/O permission bitmap
    setTssDescriptor(TSS / 8, reinterpret_cast<u64>(&tss), sizeof(Tss) - 1);

    Pointer pointer{sizeof(entries) - 1, reinterpret_cast<u64>(entries)};

    // Load the GDT, reload CS with a far return, then the data segments.
    asm volatile(
        "lgdt %0\n\t"
        "pushq %1\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "movw %w2, %%ds\n\t"
        "movw %w2, %%es\n\t"
        "movw %w2, %%ss\n\t"
        "movw %w2, %%fs\n\t"
        "movw %w2, %%gs\n\t"
        :
        : "m"(pointer), "i"(KERNEL_CODE), "r"(u64(KERNEL_DATA))
        : "rax", "memory");

    asm volatile("ltr %w0" : : "r"(u64(TSS)));
}

void setKernelStack(u64 top) {
    tss.rsp[0] = top;
}

} // namespace Rudracore::Arch::Gdt
