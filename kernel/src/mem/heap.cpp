#include "mem/heap.h"

#include "arch/x86_64/cpu.h"
#include "mem/memory.h"
#include "mem/pmm.h"
#include "panic.h"

namespace Rudracore::Heap {

namespace {

constexpr u32 MAGIC_USED = 0xB4A1A7ED;
constexpr u32 MAGIC_FREE = 0xF7EEB10C;
constexpr u32 BIG = 0xFF;

struct Header {
    u32 magic;
    u32 sizeClass; // index into BLOCK_SIZES, or BIG
    u32 size;      // requested size (for stats)
    u32 pages;     // for BIG blocks
};
static_assert(sizeof(Header) == 16);

struct FreeBlock {
    Header header;
    FreeBlock* next;
};

constexpr usize BLOCK_SIZES[] = {32, 64, 128, 256, 512, 1024, 2048};
constexpr usize CLASS_COUNT = sizeof(BLOCK_SIZES) / sizeof(BLOCK_SIZES[0]);

FreeBlock* freeLists[CLASS_COUNT];
u64 bytesInUse = 0;
u64 pagesUsed = 0;

bool refill(usize sizeClass) {
    u64 physical = Pmm::alloc();
    if (not physical)
        return false;
    pagesUsed++;

    auto* page = Mem::toVirtual<u8>(physical);
    usize blockSize = BLOCK_SIZES[sizeClass];
    for (usize offset = 0; offset + blockSize <= Mem::PAGE_SIZE; offset += blockSize) {
        auto* block = reinterpret_cast<FreeBlock*>(page + offset);
        block->header = {MAGIC_FREE, static_cast<u32>(sizeClass), 0, 0};
        block->next = freeLists[sizeClass];
        freeLists[sizeClass] = block;
    }
    return true;
}

} // namespace

void* alloc(usize size) {
    Arch::InterruptGuard guard;
    usize needed = size + sizeof(Header);

    for (usize c = 0; c < CLASS_COUNT; c++) {
        if (BLOCK_SIZES[c] < needed)
            continue;
        if (not freeLists[c] and not refill(c))
            return nullptr;

        FreeBlock* block = freeLists[c];
        freeLists[c] = block->next;
        block->header = {MAGIC_USED, static_cast<u32>(c), static_cast<u32>(size), 0};
        bytesInUse += size;
        return reinterpret_cast<u8*>(block) + sizeof(Header);
    }

    // Big allocation: whole pages.
    u64 pages = Mem::alignUp(needed, Mem::PAGE_SIZE) / Mem::PAGE_SIZE;
    u64 physical = Pmm::alloc(pages);
    if (not physical)
        return nullptr;
    pagesUsed += pages;

    auto* header = Mem::toVirtual<Header>(physical);
    *header = {MAGIC_USED, BIG, static_cast<u32>(size), static_cast<u32>(pages)};
    bytesInUse += size;
    return reinterpret_cast<u8*>(header) + sizeof(Header);
}

void free(void* ptr) {
    if (not ptr)
        return;

    Arch::InterruptGuard guard;
    auto* header = reinterpret_cast<Header*>(static_cast<u8*>(ptr) - sizeof(Header));
    if (header->magic == MAGIC_FREE)
        panic("heap: double free");
    if (header->magic != MAGIC_USED)
        panic("heap: freeing a pointer that didn't come from the heap");

    bytesInUse -= header->size;

    if (header->sizeClass == BIG) {
        u64 pages = header->pages;
        header->magic = MAGIC_FREE;
        pagesUsed -= pages;
        Pmm::free(reinterpret_cast<u64>(header) - Mem::hhdmOffset, pages);
        return;
    }

    if (header->sizeClass >= CLASS_COUNT)
        panic("heap: corrupted block header");

    auto* block = reinterpret_cast<FreeBlock*>(header);
    block->header.magic = MAGIC_FREE;
    block->next = freeLists[header->sizeClass];
    freeLists[header->sizeClass] = block;
}

Stats stats() {
    Arch::InterruptGuard guard;
    return {bytesInUse, pagesUsed};
}

} // namespace Rudracore::Heap

// C++ allocation operators for the kernel. Allocation failure panics: kernel
// code that can recover from running out of memory calls Heap::alloc directly.

using namespace Rudracore;

static void* allocOrPanic(usize size) {
    void* ptr = Heap::alloc(size);
    if (not ptr)
        panic("heap: out of memory");
    return ptr;
}

void* operator new(usize size) { return allocOrPanic(size); }
void* operator new[](usize size) { return allocOrPanic(size); }
void operator delete(void* ptr) noexcept { Heap::free(ptr); }
void operator delete[](void* ptr) noexcept { Heap::free(ptr); }
void operator delete(void* ptr, usize) noexcept { Heap::free(ptr); }
void operator delete[](void* ptr, usize) noexcept { Heap::free(ptr); }
