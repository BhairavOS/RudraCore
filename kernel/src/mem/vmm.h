#pragma once

#include "boot.h"

// Virtual memory: page tables and address spaces.
namespace Rudracore::Vmm {

enum Access : u32 {
    READ = 0, // pages are always readable
    WRITE = 1 << 0,
    EXEC = 1 << 1,
    USER = 1 << 2,
};

enum class Cache : u8 {
    WriteBack,      // normal RAM
    WriteCombining, // framebuffers: much faster for drawing
    Uncached,       // device registers
};

struct Space {
    u64 root = 0; // physical address of the top-level page table

    // Maps [virt, virt + size) to [phys, phys + size). Everything must be
    // page aligned. Uses 2 MiB pages where alignment allows. Returns false if
    // out of memory, or if part of the range is already mapped by a 2 MiB
    // page to a different physical address.
    bool map(u64 virt, u64 phys, u64 size, u32 access, Cache cache = Cache::WriteBack);

    void unmap(u64 virt, u64 size);

    // Looks up the physical address `virt` maps to. Returns false if unmapped.
    bool translate(u64 virt, u64& phys) const;

    void activate() const;
};

// Builds the kernel address space (kernel image with per-section permissions,
// plus the direct map of all RAM and the framebuffer) and switches to it.
void init(Boot::Info const& info);

Space& kernelSpace();

// Number of page-table pages allocated so far.
u64 tablePages();

} // namespace Rudracore::Vmm
