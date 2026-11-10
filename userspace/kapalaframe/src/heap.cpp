#include <kapala/heap.h>
#include <kapala/sys.h>

namespace Kapala::Heap {

namespace {

constexpr unsigned MAGIC_USED = 0x4B41504C; // "KAPL"
constexpr unsigned MAGIC_FREE = 0x46524545; // "FREE"
constexpr unsigned BIG = 0xFF;
constexpr unsigned long PAGE = 4096;
constexpr unsigned long ARENA = 64 * 1024; // small blocks are carved from these

struct Header {
    unsigned magic;
    unsigned sizeClass; // index into SIZES, or BIG
    unsigned long size; // requested size; for BIG blocks, also the usable capacity
};
static_assert(sizeof(Header) == 16);

struct FreeBlock {
    Header header;
    FreeBlock* next;
};

constexpr unsigned long SIZES[] = {32, 64, 128, 256, 512, 1024, 2048};
constexpr unsigned CLASSES = sizeof(SIZES) / sizeof(SIZES[0]);

FreeBlock* freeLists[CLASSES];
FreeBlock* freeBig; // freed big blocks, reused first-fit
unsigned long bytesInUse;
unsigned long bytesFromKernel;

unsigned char* arena;
unsigned long arenaLeft;

void* fromKernel(unsigned long size) {
    void* memory = Sys::memoryAlloc(size);
    if (memory)
        bytesFromKernel += size;
    return memory;
}

// Carves a page from the current arena into blocks of one size.
bool refill(unsigned c) {
    if (arenaLeft < PAGE) {
        arena = static_cast<unsigned char*>(fromKernel(ARENA));
        if (not arena)
            return false;
        arenaLeft = ARENA;
    }
    unsigned char* page = arena;
    arena += PAGE;
    arenaLeft -= PAGE;

    for (unsigned long offset = 0; offset + SIZES[c] <= PAGE; offset += SIZES[c]) {
        auto* block = reinterpret_cast<FreeBlock*>(page + offset);
        block->header = {MAGIC_FREE, c, 0};
        block->next = freeLists[c];
        freeLists[c] = block;
    }
    return true;
}

void* allocBig(unsigned long size) {
    for (FreeBlock** link = &freeBig; *link; link = &(*link)->next) {
        FreeBlock* block = *link;
        if (block->header.size >= size) {
            *link = block->next;
            block->header.magic = MAGIC_USED;
            bytesInUse += block->header.size;
            return reinterpret_cast<unsigned char*>(block) + sizeof(Header);
        }
    }

    unsigned long total = (size + sizeof(Header) + PAGE - 1) & ~(PAGE - 1);
    auto* header = static_cast<Header*>(fromKernel(total));
    if (not header)
        return nullptr;
    *header = {MAGIC_USED, BIG, total - sizeof(Header)};
    bytesInUse += header->size;
    return reinterpret_cast<unsigned char*>(header) + sizeof(Header);
}

} // namespace

void* alloc(unsigned long size) {
    unsigned long needed = size + sizeof(Header);
    for (unsigned c = 0; c < CLASSES; c++) {
        if (SIZES[c] < needed)
            continue;
        if (not freeLists[c] and not refill(c))
            return nullptr;

        FreeBlock* block = freeLists[c];
        freeLists[c] = block->next;
        block->header = {MAGIC_USED, c, size};
        bytesInUse += size;
        return reinterpret_cast<unsigned char*>(block) + sizeof(Header);
    }
    return allocBig(size);
}

void free(void* pointer) {
    if (not pointer)
        return;

    auto* header = reinterpret_cast<Header*>(static_cast<unsigned char*>(pointer) - sizeof(Header));
    if (header->magic != MAGIC_USED) {
        Sys::log(header->magic == MAGIC_FREE ? "heap: double free" : "heap: freeing a bad pointer");
        Sys::exit(-1);
    }

    bytesInUse -= header->size;
    header->magic = MAGIC_FREE;
    auto* block = reinterpret_cast<FreeBlock*>(header);
    if (header->sizeClass == BIG) {
        block->next = freeBig;
        freeBig = block;
    } else {
        block->next = freeLists[header->sizeClass];
        freeLists[header->sizeClass] = block;
    }
}

Stats stats() {
    return {bytesInUse, bytesFromKernel};
}

} // namespace Kapala::Heap

namespace {

void* allocOrExit(unsigned long size) {
    void* pointer = Kapala::Heap::alloc(size);
    if (not pointer) {
        Kapala::Sys::log("out of memory");
        Kapala::Sys::exit(-1);
    }
    return pointer;
}

} // namespace

void* operator new(unsigned long size) { return allocOrExit(size); }
void* operator new[](unsigned long size) { return allocOrExit(size); }
void operator delete(void* p) noexcept { Kapala::Heap::free(p); }
void operator delete[](void* p) noexcept { Kapala::Heap::free(p); }
void operator delete(void* p, unsigned long) noexcept { Kapala::Heap::free(p); }
void operator delete[](void* p, unsigned long) noexcept { Kapala::Heap::free(p); }
