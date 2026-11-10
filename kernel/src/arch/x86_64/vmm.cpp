// x86_64 4-level paging implementation of Vmm.

#include "mem/vmm.h"

#include "arch/x86_64/cpu.h"
#include "log.h"
#include "mem/memory.h"
#include "mem/pmm.h"
#include "panic.h"

// Section boundaries from linker.ld.
extern "C" u8 __requests_start[], __requests_end[];
extern "C" u8 __text_start[], __text_end[];
extern "C" u8 __rodata_start[], __rodata_end[];
extern "C" u8 __data_start[], __data_end[];

namespace Rudracore::Vmm {

namespace {

constexpr u64 PRESENT = 1ull << 0;
constexpr u64 WRITABLE = 1ull << 1;
constexpr u64 USER_PAGE = 1ull << 2;
constexpr u64 PWT = 1ull << 3;
constexpr u64 PCD = 1ull << 4;
constexpr u64 HUGE = 1ull << 7;     // in a page-directory entry: 2 MiB page
constexpr u64 PAT_4K = 1ull << 7;   // in a page-table entry
constexpr u64 PAT_2M = 1ull << 12;  // in a 2 MiB page-directory entry
constexpr u64 OWNED_PAGE = 1ull << 9; // software bit: free the page with its space
constexpr u64 NO_EXECUTE = 1ull << 63;

constexpr u64 ADDRESS_MASK = 0x000ffffffffff000;
constexpr u64 HUGE_ADDRESS_MASK = 0x000fffffffe00000;

constexpr u64 SIZE_4K = 0x1000;
constexpr u64 SIZE_2M = 0x200000;

Space kernel;
u64 tablesAllocated = 0;

u64* table(u64 physical) {
    return Mem::toVirtual<u64>(physical);
}

// Level 3 = PML4, 2 = PDPT, 1 = page directory, 0 = page table.
usize index(u64 virt, int level) {
    return (virt >> (12 + 9 * level)) & 511;
}

// Returns the table an entry points to, creating it if needed. Returns
// nullptr if allocation fails or the entry is a huge page.
u64* nextTable(u64* tbl, usize i, bool create, bool user) {
    u64 entry = tbl[i];
    if (entry & PRESENT) {
        if (entry & HUGE)
            return nullptr;
        if (user)
            tbl[i] |= USER_PAGE;
        return table(entry & ADDRESS_MASK);
    }

    if (not create)
        return nullptr;

    u64 physical = Pmm::allocZeroed();
    if (not physical)
        return nullptr;
    tablesAllocated++;

    // Intermediate levels are permissive; the leaf entry decides access.
    tbl[i] = physical | PRESENT | WRITABLE | (user ? USER_PAGE : 0);
    return table(physical);
}

// The bootloader programs the PAT as: 0 WB, 1 WT, 2 UC-, 3 UC, 4 WP, 5 WC.
// A page selects an entry with its PAT, PCD and PWT bits.
u64 leafFlags(u32 access, Cache cache, bool huge) {
    u64 flags = PRESENT;
    if (access & WRITE)
        flags |= WRITABLE;
    if (access & USER)
        flags |= USER_PAGE;
    if (access & OWNED)
        flags |= OWNED_PAGE;
    if (not(access & EXEC))
        flags |= NO_EXECUTE;

    switch (cache) {
    case Cache::WriteBack:
        break;
    case Cache::WriteCombining: // PAT entry 5
        flags |= (huge ? PAT_2M : PAT_4K) | PWT;
        break;
    case Cache::Uncached: // PAT entry 3
        flags |= PCD | PWT;
        break;
    }

    if (huge)
        flags |= HUGE;
    return flags;
}

void mapKernelSection(Boot::Info const& info, u8* start, u8* end, u32 access, char const* name) {
    u64 virt = Mem::alignDown(reinterpret_cast<u64>(start), SIZE_4K);
    u64 size = Mem::alignUp(reinterpret_cast<u64>(end), SIZE_4K) - virt;
    if (size == 0)
        return;

    u64 phys = virt - info.kernelVirtualBase + info.kernelPhysicalBase;
    if (not kernel.map(virt, phys, size, access))
        panic("vmm: failed to map the kernel image");

    log("[vmm]  kernel ", name, " ", Hex{virt}, " ", Bytes{size}, " ",
        (access & WRITE) ? "rw-" : ((access & EXEC) ? "r-x" : "r--"));
}

bool isDirectMapped(Boot::RegionType type) {
    using enum Boot::RegionType;
    switch (type) {
    case Usable:
    case BootloaderReclaimable:
    case Kernel:
    case Framebuffer:
    case AcpiReclaimable:
    case AcpiNvs:
    case ReservedMapped:
        return true;
    case Reserved:
    case BadMemory:
        return false;
    }
    return false;
}

} // namespace

bool Space::map(u64 virt, u64 phys, u64 size, u32 access, Cache cache) {
    if ((virt | phys | size) & (SIZE_4K - 1))
        return false;

    bool user = access & USER;
    u64* pml4 = table(root);

    for (u64 offset = 0; offset < size;) {
        u64 v = virt + offset;
        u64 p = phys + offset;

        u64* pdpt = nextTable(pml4, index(v, 3), true, user);
        if (not pdpt)
            return false;

        u64* pd = nextTable(pdpt, index(v, 2), true, user);
        if (not pd)
            return false;

        u64& pde = pd[index(v, 1)];
        bool pdeIsTable = (pde & PRESENT) and not(pde & HUGE);
        bool canUseHuge = ((v | p) & (SIZE_2M - 1)) == 0 and size - offset >= SIZE_2M;

        if (canUseHuge and not pdeIsTable) {
            pde = p | leafFlags(access, cache, true);
            Arch::invalidatePage(v);
            offset += SIZE_2M;
            continue;
        }

        if ((pde & PRESENT) and (pde & HUGE)) {
            // Already covered by a 2 MiB page: fine only if it maps the same memory.
            if ((pde & HUGE_ADDRESS_MASK) + (v & (SIZE_2M - 1)) != p)
                return false;
            offset += SIZE_4K;
            continue;
        }

        u64* pt = nextTable(pd, index(v, 1), true, user);
        if (not pt)
            return false;

        pt[index(v, 0)] = p | leafFlags(access, cache, false);
        Arch::invalidatePage(v);
        offset += SIZE_4K;
    }

    return true;
}

void Space::unmap(u64 virt, u64 size) {
    u64* pml4 = table(root);
    for (u64 offset = 0; offset < size;) {
        u64 v = virt + offset;

        u64* pdpt = nextTable(pml4, index(v, 3), false, false);
        u64* pd = pdpt ? nextTable(pdpt, index(v, 2), false, false) : nullptr;
        if (not pd) {
            offset += SIZE_4K;
            continue;
        }

        u64& pde = pd[index(v, 1)];
        if ((pde & PRESENT) and (pde & HUGE)) {
            // Splitting 2 MiB pages isn't supported: only whole ones are removed.
            if ((v & (SIZE_2M - 1)) == 0 and size - offset >= SIZE_2M) {
                pde = 0;
                Arch::invalidatePage(v);
                offset += SIZE_2M;
            } else {
                log("[vmm]  warning: can't unmap part of a 2 MiB page at ", Hex{v});
                offset += SIZE_4K;
            }
            continue;
        }

        if (u64* pt = nextTable(pd, index(v, 1), false, false)) {
            pt[index(v, 0)] = 0;
            Arch::invalidatePage(v);
        }
        offset += SIZE_4K;
    }
}

bool Space::translate(u64 virt, u64& phys) const {
    u64* pml4 = table(root);
    u64* pdpt = nextTable(pml4, index(virt, 3), false, false);
    if (not pdpt)
        return false;
    u64* pd = nextTable(pdpt, index(virt, 2), false, false);
    if (not pd)
        return false;

    u64 pde = pd[index(virt, 1)];
    if (not(pde & PRESENT))
        return false;
    if (pde & HUGE) {
        phys = (pde & HUGE_ADDRESS_MASK) + (virt & (SIZE_2M - 1));
        return true;
    }

    u64 pte = table(pde & ADDRESS_MASK)[index(virt, 0)];
    if (not(pte & PRESENT))
        return false;
    phys = (pte & ADDRESS_MASK) + (virt & (SIZE_4K - 1));
    return true;
}

void Space::activate() const {
    Arch::writeCr3(root);
}

void init(Boot::Info const& info) {
    kernel.root = Pmm::allocZeroed();
    if (not kernel.root)
        panic("vmm: out of memory for the kernel page table");
    tablesAllocated++;

    // Create every upper-half PML4 entry now. Future address spaces copy
    // these 256 entries, so kernel mappings added later appear everywhere.
    u64* pml4 = table(kernel.root);
    for (usize i = 256; i < 512; i++) {
        if (not nextTable(pml4, i, true, false))
            panic("vmm: out of memory for the kernel page tables");
    }

    // Kernel image: code is read+execute, constants read-only, data read+write.
    // Nothing is both writable and executable.
    mapKernelSection(info, __requests_start, __requests_end, READ, "boot  ");
    mapKernelSection(info, __text_start, __text_end, READ | EXEC, "text  ");
    mapKernelSection(info, __rodata_start, __rodata_end, READ, "rodata");
    mapKernelSection(info, __data_start, __data_end, READ | WRITE, "data  ");

    // Direct map of physical memory, at the same offset the bootloader used,
    // so pointers into it (like the framebuffer) stay valid.
    u64 mapped = 0;
    for (usize i = 0; i < info.regionCount; i++) {
        auto const& r = info.regions[i];
        if (not isDirectMapped(r.type))
            continue;

        u64 base = Mem::alignDown(r.base, SIZE_4K);
        u64 end = Mem::alignUp(r.base + r.length, SIZE_4K);
        Cache cache = r.type == Boot::RegionType::Framebuffer ? Cache::WriteCombining : Cache::WriteBack;
        if (not kernel.map(info.hhdmOffset + base, base, end - base, READ | WRITE, cache))
            panic("vmm: failed to build the direct map");
        mapped += end - base;
    }
    log("[vmm]  direct map ", Bytes{mapped}, " at ", Hex{info.hhdmOffset});

    kernel.activate();
}

Space& kernelSpace() {
    return kernel;
}

Space* createUserSpace() {
    u64 root = Pmm::allocZeroed();
    if (not root)
        return nullptr;
    tablesAllocated++;

    // Share the kernel half: the same 256 upper PML4 entries everywhere.
    u64* pml4 = table(root);
    u64* kernelPml4 = table(kernel.root);
    for (usize i = 256; i < 512; i++)
        pml4[i] = kernelPml4[i];

    Space* space = new Space{};
    space->root = root;
    return space;
}

namespace {

// Frees a user page table at `level` (2 = PDPT, 1 = PD, 0 = PT), everything
// below it, and the OWNED pages it maps.
void freeTable(u64 physical, int level) {
    u64* tbl = table(physical);
    for (usize i = 0; i < 512; i++) {
        u64 entry = tbl[i];
        if (not(entry & PRESENT))
            continue;

        bool isLeaf = level == 0 or (level == 1 and (entry & HUGE));
        if (isLeaf) {
            if (entry & OWNED_PAGE) {
                u64 pages = level == 0 ? 1 : 512;
                u64 mask = level == 0 ? ADDRESS_MASK : HUGE_ADDRESS_MASK;
                Pmm::free(entry & mask, pages);
            }
        } else {
            freeTable(entry & ADDRESS_MASK, level - 1);
        }
    }
    Pmm::free(physical);
    tablesAllocated--;
}

} // namespace

void destroyUserSpace(Space* space) {
    if (not space)
        return;
    if (Arch::readCr3() == space->root)
        panic("vmm: destroying the active address space");

    u64* pml4 = table(space->root);
    for (usize i = 0; i < 256; i++) {
        if (pml4[i] & PRESENT)
            freeTable(pml4[i] & ADDRESS_MASK, 2);
    }
    Pmm::free(space->root);
    tablesAllocated--;
    delete space;
}

bool isUserRange(Space const& space, u64 address, u64 length) {
    u64 end = address + length;
    if (end < address or end > USER_END)
        return false;

    for (u64 page = Mem::alignDown(address, SIZE_4K); page < end; page += SIZE_4K) {
        u64 phys;
        if (not space.translate(page, phys))
            return false;
    }
    return true;
}

u64 tablePages() {
    return tablesAllocated;
}

} // namespace Rudracore::Vmm
