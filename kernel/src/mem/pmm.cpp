#include "mem/pmm.h"

#include "arch/x86_64/cpu.h"
#include "mem/memory.h"
#include "panic.h"

extern "C" void* memset(void* dest, int value, usize n);

namespace Rudracore::Pmm {

namespace {

u64* bitmap = nullptr;
u64 pageCount = 0; // pages covered by the bitmap
u64 managedPages = 0;
u64 freePages = 0;
u64 bitmapBytes = 0;
u64 searchHint = 0; // next-fit: resume searching after the last allocation

bool isUsed(u64 page) {
    return (bitmap[page / 64] >> (page % 64)) & 1;
}

void setUsed(u64 page) {
    if (not isUsed(page)) {
        bitmap[page / 64] |= 1ull << (page % 64);
        freePages--;
    }
}

void setFree(u64 page) {
    if (isUsed(page)) {
        bitmap[page / 64] &= ~(1ull << (page % 64));
        freePages++;
    }
}

// Gives the whole pages inside [base, base + length) to the allocator.
void addFreeRange(u64 base, u64 length) {
    u64 first = Mem::alignUp(base, Mem::PAGE_SIZE) / Mem::PAGE_SIZE;
    u64 last = min(Mem::alignDown(base + length, Mem::PAGE_SIZE) / Mem::PAGE_SIZE, pageCount);
    for (u64 page = first; page < last; page++) {
        if (isUsed(page)) {
            setFree(page);
            managedPages++;
        }
    }
}

void markUsedRange(u64 base, u64 length) {
    u64 first = Mem::alignDown(base, Mem::PAGE_SIZE) / Mem::PAGE_SIZE;
    u64 last = min(Mem::alignUp(base + length, Mem::PAGE_SIZE) / Mem::PAGE_SIZE, pageCount);
    for (u64 page = first; page < last; page++)
        setUsed(page);
}

bool isAllocatable(Boot::RegionType type) {
    return type == Boot::RegionType::Usable or
           type == Boot::RegionType::BootloaderReclaimable;
}

} // namespace

void init(Boot::Info const& info) {
    u64 highest = 0;
    for (usize i = 0; i < info.regionCount; i++) {
        auto const& r = info.regions[i];
        if (isAllocatable(r.type))
            highest = max(highest, r.base + r.length);
    }

    pageCount = Mem::alignUp(highest, Mem::PAGE_SIZE) / Mem::PAGE_SIZE;
    bitmapBytes = Mem::alignUp((pageCount + 63) / 64 * 8, Mem::PAGE_SIZE);

    u64 bitmapPhysical = 0;
    for (usize i = 0; i < info.regionCount; i++) {
        auto const& r = info.regions[i];
        u64 start = Mem::alignUp(r.base, Mem::PAGE_SIZE);
        if (r.type == Boot::RegionType::Usable and start != 0 and
            start + bitmapBytes <= r.base + r.length) {
            bitmapPhysical = start;
            break;
        }
    }
    if (bitmapPhysical == 0)
        panic("pmm: no usable memory region is big enough for the page bitmap");

    // Start with everything used, then free only what's really usable.
    bitmap = Mem::toVirtual<u64>(bitmapPhysical);
    memset(bitmap, 0xFF, bitmapBytes);
    freePages = 0;

    for (usize i = 0; i < info.regionCount; i++) {
        auto const& r = info.regions[i];
        if (r.type == Boot::RegionType::Usable)
            addFreeRange(r.base, r.length);
    }

    markUsedRange(bitmapPhysical, bitmapBytes);
    setUsed(0); // physical address 0 means "allocation failed"
}

u64 alloc(usize count) {
    Arch::InterruptGuard guard;
    if (count == 0 or count > freePages)
        return 0;

    u64 run = 0, runStart = 0;
    for (u64 n = 0; n < pageCount; n++) {
        u64 page = (searchHint + n) % pageCount;
        if (page == 0)
            run = 0; // a run can't wrap around the end of memory

        // Skip fully used 64-page blocks quickly.
        if (run == 0 and page % 64 == 0 and bitmap[page / 64] == ~0ull and page + 64 <= pageCount) {
            n += 63;
            continue;
        }

        if (isUsed(page)) {
            run = 0;
            continue;
        }

        if (run == 0)
            runStart = page;
        if (++run == count) {
            for (u64 p = runStart; p < runStart + count; p++)
                setUsed(p);
            searchHint = runStart + count;
            return runStart * Mem::PAGE_SIZE;
        }
    }
    return 0;
}

u64 allocZeroed(usize count) {
    u64 physical = alloc(count);
    if (physical)
        memset(Mem::toVirtual(physical), 0, count * Mem::PAGE_SIZE);
    return physical;
}

void free(u64 physical, usize count) {
    Arch::InterruptGuard guard;
    if (physical % Mem::PAGE_SIZE != 0)
        panic("pmm: freeing a misaligned address");

    u64 first = physical / Mem::PAGE_SIZE;
    if (first == 0 or first + count > pageCount)
        panic("pmm: freeing memory outside of the managed range");

    for (u64 page = first; page < first + count; page++) {
        if (not isUsed(page))
            panic("pmm: double free");
        setFree(page);
    }
}

void reclaimBootloaderMemory(Boot::Info const& info) {
    for (usize i = 0; i < info.regionCount; i++) {
        auto const& r = info.regions[i];
        if (r.type == Boot::RegionType::BootloaderReclaimable)
            addFreeRange(r.base, r.length);
    }
}

Stats stats() {
    return {
        .managedPages = managedPages,
        .freePages = freePages,
        .bitmapBytes = bitmapBytes,
    };
}

bool userCanUse(u64 pages) {
    constexpr u64 KERNEL_RESERVE_PAGES = 1024; // 4 MiB
    Arch::InterruptGuard guard;
    return freePages >= KERNEL_RESERVE_PAGES and pages <= freePages - KERNEL_RESERVE_PAGES;
}

} // namespace Rudracore::Pmm
