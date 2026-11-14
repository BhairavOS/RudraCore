// Page tables, the final memory map, and the jump into the kernel.

#include "loader.h"

namespace Loader {

namespace {

constexpr uint64_t PRESENT = 1 << 0;
constexpr uint64_t WRITABLE = 1 << 1;
constexpr uint64_t HUGE = 1 << 7;
constexpr uint64_t ADDRESS_MASK = 0x000ffffffffff000;

constexpr uint64_t SIZE_2M = 0x200000;
constexpr uint64_t SIZE_1G = 0x40000000;

// MARK: Memory map snapshot

struct MemoryMap {
    uint8_t* buffer = nullptr;
    uint64_t capacity = 0;
    uint64_t size = 0;
    uint64_t key = 0;
    uint64_t descriptorSize = 0;

    uint64_t count() const { return descriptorSize ? size / descriptorSize : 0; }

    Efi::MemoryDescriptor const& at(uint64_t i) const {
        return *reinterpret_cast<Efi::MemoryDescriptor const*>(buffer + i * descriptorSize);
    }
};

// Gets the current memory map, growing the buffer if needed. Allocating the
// buffer changes the map, so it's made a bit bigger than asked.
Efi::Status getMemoryMap(MemoryMap& map) {
    for (;;) {
        map.size = map.capacity;
        uint32_t version = 0;
        Efi::Status status = bootServices->getMemoryMap(
            &map.size, reinterpret_cast<Efi::MemoryDescriptor*>(map.buffer), &map.key,
            &map.descriptorSize, &version);
        if (status != Efi::BUFFER_TOO_SMALL)
            return status;

        if (map.buffer)
            bootServices->freePool(map.buffer);
        map.capacity = map.size + 16 * (map.descriptorSize ? map.descriptorSize : 64);
        if (Efi::failed(bootServices->allocatePool(Efi::LOADER_DATA, map.capacity,
                                                   reinterpret_cast<void**>(&map.buffer))))
            fail("out of memory for the memory map");
    }
}

// MARK: Page tables

struct PageTables {
    uint64_t pool;     // pre-allocated table pages
    uint64_t poolSize; // in pages
    uint64_t used;
    uint64_t pml4;

    uint64_t* newTable() {
        if (used == poolSize)
            fail("ran out of page-table memory");
        return reinterpret_cast<uint64_t*>(pool + PAGE_SIZE * used++);
    }

    uint64_t* next(uint64_t* table, uint64_t index) {
        if (not(table[index] & PRESENT))
            table[index] = reinterpret_cast<uint64_t>(newTable()) | PRESENT | WRITABLE;
        return reinterpret_cast<uint64_t*>(table[index] & ADDRESS_MASK);
    }

    static uint64_t index(uint64_t virt, int level) { return (virt >> (12 + 9 * level)) & 511; }

    void map2M(uint64_t virt, uint64_t phys) {
        auto* pml4Table = reinterpret_cast<uint64_t*>(pml4);
        uint64_t* pd = next(next(pml4Table, index(virt, 3)), index(virt, 2));
        pd[index(virt, 1)] = phys | PRESENT | WRITABLE | HUGE;
    }

    void map4K(uint64_t virt, uint64_t phys) {
        auto* pml4Table = reinterpret_cast<uint64_t*>(pml4);
        uint64_t* pt = next(next(next(pml4Table, index(virt, 3)), index(virt, 2)), index(virt, 1));
        pt[index(virt, 0)] = phys | PRESENT | WRITABLE;
    }

    // Identity map + direct map of [base, end), in 2 MiB pages.
    void mapPhysical(uint64_t base, uint64_t end) {
        for (uint64_t a = alignDown(base, SIZE_2M); a < alignUp(end, SIZE_2M); a += SIZE_2M) {
            map2M(a, a);
            map2M(Shool::HHDM_OFFSET + a, a);
        }
    }
};

// Enough table pages for the ranges to map: per 1 GiB touched, a page
// directory for each of the two mappings, plus the upper levels.
uint64_t tablePagesNeeded(MemoryMap const& map, Kernel const& kernel) {
    uint64_t pages = 16 + kernel.size / SIZE_2M + 2;
    for (uint64_t i = 0; i < map.count(); i++) {
        auto const& d = map.at(i);
        uint64_t gigabytes = (d.numberOfPages * PAGE_SIZE) / SIZE_1G + 2;
        pages += 2 * gigabytes + 4;
    }
    return pages + 8; // framebuffer
}

// MARK: Memory map conversion

uint32_t regionType(uint32_t efiType) {
    switch (efiType) {
    case Efi::CONVENTIONAL_MEMORY:
    case Efi::BOOT_SERVICES_CODE:
    case Efi::BOOT_SERVICES_DATA:
    case Efi::LOADER_CODE: // ShoolLoader itself: gone once the kernel runs
    case Efi::LOADER_DATA:
        return Shool::REGION_USABLE;
    case Efi::SHOOL_KERNEL: return Shool::REGION_KERNEL;
    case Efi::SHOOL_RECLAIMABLE: return Shool::REGION_LOADER_RECLAIMABLE;
    case Efi::ACPI_RECLAIM_MEMORY: return Shool::REGION_ACPI_RECLAIMABLE;
    case Efi::ACPI_MEMORY_NVS: return Shool::REGION_ACPI_NVS;
    case Efi::UNUSABLE_MEMORY: return Shool::REGION_BAD_MEMORY;
    default: return Shool::REGION_RESERVED; // runtime services, MMIO, ...
    }
}

// Sorted by address, with touching regions of the same type merged.
void convertMemoryMap(MemoryMap const& map, Shool::BootInfo& info) {
    info.regionCount = 0;
    for (uint64_t i = 0; i < map.count(); i++) {
        auto const& d = map.at(i);
        Shool::MemoryRegion region{d.physicalStart, d.numberOfPages * PAGE_SIZE, regionType(d.type), 0};
        if (region.length == 0)
            continue;

        // Insertion sort: firmware maps are short and nearly sorted.
        uint64_t pos = info.regionCount;
        while (pos > 0 and info.regions[pos - 1].base > region.base)
            pos--;
        if (info.regionCount == Shool::BootInfo::MAX_REGIONS)
            break;
        for (uint64_t j = info.regionCount; j > pos; j--)
            info.regions[j] = info.regions[j - 1];
        info.regions[pos] = region;
        info.regionCount++;
    }

    uint64_t merged = 0;
    for (uint64_t i = 0; i < info.regionCount; i++) {
        auto& last = info.regions[merged > 0 ? merged - 1 : 0];
        auto const& r = info.regions[i];
        if (merged > 0 and last.type == r.type and last.base + last.length == r.base)
            last.length += r.length;
        else
            info.regions[merged++] = r;
    }
    info.regionCount = merged;

    // The framebuffer usually isn't in the memory map: add it.
    auto const& fb = info.framebuffer;
    if (fb.physical and info.regionCount < Shool::BootInfo::MAX_REGIONS) {
        info.regions[info.regionCount++] = {fb.physical, alignUp(fb.pitch * fb.height, PAGE_SIZE),
                                            Shool::REGION_FRAMEBUFFER, 0};
    }
}

// MARK: CPU setup

uint64_t readMsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (uint64_t(hi) << 32) | lo;
}

void writeMsr(uint32_t msr, uint64_t value) {
    asm volatile("wrmsr" : : "c"(msr), "a"(uint32_t(value)), "d"(uint32_t(value >> 32)));
}

void cpuid(uint32_t leaf, uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
    asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(leaf), "c"(0));
}

constexpr uint32_t MSR_PAT = 0x277;
constexpr uint32_t MSR_EFER = 0xC0000080;
constexpr uint64_t EFER_NXE = 1 << 11;
constexpr uint64_t CR0_WP = 1 << 16;

// 0 WB, 1 WT, 2 UC-, 3 UC, 4 WP, 5 WC, 6 UC-, 7 UC: what the protocol promises.
constexpr uint64_t PAT_LAYOUT = 0x0007010500070406;

void setupCpu() {
    uint32_t a, b, c, d;
    cpuid(0x80000001, a, b, c, d);
    if (d & (1 << 20)) // no-execute bit supported
        writeMsr(MSR_EFER, readMsr(MSR_EFER) | EFER_NXE);

    cpuid(1, a, b, c, d);
    if (d & (1 << 16)) // PAT supported
        writeMsr(MSR_PAT, PAT_LAYOUT);

    uint64_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    asm volatile("mov %0, %%cr0" : : "r"(cr0 | CR0_WP));
}

// Where `virt` leads in the page tables at `pml4`, or ~0 if unmapped.
uint64_t translate(uint64_t pml4, uint64_t virt) {
    auto* table = reinterpret_cast<uint64_t*>(pml4);
    for (int level = 3; level >= 0; level--) {
        uint64_t e = table[PageTables::index(virt, level)];
        if (not(e & PRESENT))
            return ~0ull;
        if (level == 1 and (e & HUGE))
            return (e & ADDRESS_MASK & ~(SIZE_2M - 1)) + (virt & (SIZE_2M - 1));
        if (level == 0)
            return (e & ADDRESS_MASK) + (virt & (PAGE_SIZE - 1));
        table = reinterpret_cast<uint64_t*>(e & ADDRESS_MASK);
    }
    return ~0ull;
}

void expectMapped(uint64_t pml4, uint64_t virt, uint64_t physical, char const* what) {
    if (translate(pml4, virt) != physical) {
        print("  not mapped as promised: ");
        print(what);
        print("\n");
        fail("the kernel's first page tables are wrong");
    }
}

} // namespace

void checkHandover(uint64_t pml4, Kernel const& kernel, uint64_t stack, uint64_t stackSize, Shool::BootInfo const* info) {
    uint32_t a, b, c, d;
    cpuid(0x80000000, a, b, c, d);
    uint32_t highest = a;
    cpuid(0x80000001, a, b, c, d);
    if (highest < 0x80000001 or not(d & (1 << 20)))
        fail("this CPU has no no-execute (NX) bit, which RudraCore needs");

    // Everything the jump and the kernel's first instructions rely on.
    for (uint64_t offset = 0; offset < kernel.size; offset += PAGE_SIZE)
        expectMapped(pml4, kernel.virtualBase + offset, kernel.physical + offset, "the kernel image");
    expectMapped(pml4, kernel.entry, kernel.physical + (kernel.entry - kernel.virtualBase), "the kernel's entry point");
    for (uint64_t offset = 0; offset < stackSize; offset += PAGE_SIZE)
        expectMapped(pml4, Shool::HHDM_OFFSET + stack + offset, stack + offset, "the kernel's stack");
    auto infoPhysical = reinterpret_cast<uint64_t>(info);
    expectMapped(pml4, Shool::HHDM_OFFSET + infoPhysical, infoPhysical, "the boot information");
    expectMapped(pml4, Shool::HHDM_OFFSET + infoPhysical + sizeof(*info) - 1, infoPhysical + sizeof(*info) - 1,
                 "the end of the boot information");
    // The code switching CR3 keeps running at its own (identity) address.
    auto self = reinterpret_cast<uint64_t>(&enterKernel);
    expectMapped(pml4, self, self, "the loader's hand-over code");
    for (uint32_t i = 0; i < info->moduleCount; i++)
        expectMapped(pml4, Shool::HHDM_OFFSET + info->modules[i].physical, info->modules[i].physical, "a module");
    auto const& fb = info->framebuffer;
    if (fb.physical)
        expectMapped(pml4, Shool::HHDM_OFFSET + fb.physical, fb.physical, "the framebuffer");
}

uint64_t buildPageTables(Kernel const& kernel, Shool::Framebuffer const& fb) {
    MemoryMap map;
    if (Efi::failed(getMemoryMap(map)))
        fail("can't read the memory map");

    uint64_t poolSize = tablePagesNeeded(map, kernel);
    PageTables tables{allocatePages(poolSize, Efi::SHOOL_RECLAIMABLE), poolSize, 0, 0};
    tables.pml4 = reinterpret_cast<uint64_t>(tables.newTable());

    // Every region the firmware knows about (memory allocated from now on
    // comes out of these), plus the framebuffer.
    for (uint64_t i = 0; i < map.count(); i++) {
        auto const& d = map.at(i);
        tables.mapPhysical(d.physicalStart, d.physicalStart + d.numberOfPages * PAGE_SIZE);
    }
    if (fb.physical)
        tables.mapPhysical(fb.physical, fb.physical + fb.pitch * fb.height);

    for (uint64_t offset = 0; offset < kernel.size; offset += PAGE_SIZE)
        tables.map4K(kernel.virtualBase + offset, kernel.physical + offset);

    bootServices->freePool(map.buffer);
    return tables.pml4;
}

void exitBootServices(Shool::BootInfo& info) {
    MemoryMap map;
    if (Efi::failed(getMemoryMap(map)))
        fail("can't read the memory map");

    // ExitBootServices fails if the map changed since we read it (firmware
    // timers can allocate): read it again, without allocating, and retry.
    Efi::Status status = bootServices->exitBootServices(imageHandle, map.key);
    for (int attempt = 0; Efi::failed(status) and attempt < 4; attempt++) {
        map.size = map.capacity;
        uint32_t version = 0;
        bootServices->getMemoryMap(&map.size, reinterpret_cast<Efi::MemoryDescriptor*>(map.buffer),
                                   &map.key, &map.descriptorSize, &version);
        status = bootServices->exitBootServices(imageHandle, map.key);
    }
    if (Efi::failed(status))
        fail("the firmware refused to hand over the machine", status);

    // From here on: no firmware services, no printing.
    convertMemoryMap(map, info);
}

void enterKernel(uint64_t pml4, uint64_t stackTop, uint64_t entry, Shool::BootInfo* info) {
    setupCpu();
    asm volatile(
        "cli\n\t"
        "mov %%rax, %%cr3\n\t"
        "mov %%rbx, %%rsp\n\t"
        "xor %%ebp, %%ebp\n\t"
        "pushq $0\n\t" // fake return address: rsp % 16 == 8 on entry, as for a call
        "jmp *%%rcx\n\t"
        :
        : "a"(pml4), "b"(stackTop), "c"(entry), "D"(Shool::MAGIC), "S"(info)
        : "memory");
    __builtin_unreachable();
}

} // namespace Loader
