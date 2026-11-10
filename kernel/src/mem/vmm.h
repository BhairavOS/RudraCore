#pragma once

#include "boot.h"

// Virtual memory: page tables and address spaces.
namespace Rudracore::Vmm {

enum Access : u32 {
    READ = 0, // pages are always readable
    WRITE = 1 << 0,
    EXEC = 1 << 1,
    USER = 1 << 2,
    OWNED = 1 << 3, // the physical pages belong to the space and are freed with it
};

// User address spaces use the lower half; the upper half is the kernel's.
constexpr u64 USER_END = 0x0000800000000000;

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

    // Like translate(), also telling whether the page is writable.
    bool query(u64 virt, u64& phys, bool& writable) const;

    void activate() const;
};

// Builds the kernel address space (kernel image with per-section permissions,
// plus the direct map of all RAM and the framebuffer) and switches to it.
void init(Boot::Info const& info);

Space& kernelSpace();

// A new address space with an empty user half that shares the kernel half.
// Returns nullptr when out of memory.
Space* createUserSpace();

// Frees the user half's page tables, every OWNED page, and the space itself.
// The space must not be active.
void destroyUserSpace(Space* space);

// True if [address, address + length) is in the user half and fully mapped
// (and writable, if asked). Checked before the kernel touches user memory.
bool isUserRange(Space const& space, u64 address, u64 length, bool writable = false);

// Number of page-table pages allocated so far.
u64 tablePages();

} // namespace Rudracore::Vmm
