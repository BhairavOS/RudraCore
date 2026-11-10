#include "loader.h"

namespace Loader {

namespace {

struct ElfHeader {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t programHeaderOffset;
    uint64_t sectionHeaderOffset;
    uint32_t flags;
    uint16_t headerSize;
    uint16_t programHeaderSize;
    uint16_t programHeaderCount;
    uint16_t sectionHeaderSize;
    uint16_t sectionHeaderCount;
    uint16_t sectionNameIndex;
};

struct ElfProgramHeader {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t fileSize;
    uint64_t memorySize;
    uint64_t align;
};

constexpr uint32_t PT_LOAD = 1;

// The kernel must live in the top 2 GiB (it's built with -mcmodel=kernel).
constexpr uint64_t KERNEL_MIN = 0xffffffff80000000;

} // namespace

Kernel loadKernel(FileData const& file) {
    auto const* h = reinterpret_cast<ElfHeader const*>(file.data);
    if (file.size < sizeof(ElfHeader) or h->ident[0] != 0x7F or h->ident[1] != 'E' or
        h->ident[2] != 'L' or h->ident[3] != 'F' or h->ident[4] != 2 or h->machine != 62 or h->type != 2)
        fail("the kernel isn't a 64-bit x86_64 ELF executable");

    if (h->programHeaderSize != sizeof(ElfProgramHeader) or
        h->programHeaderOffset > file.size or
        h->programHeaderCount > (file.size - h->programHeaderOffset) / sizeof(ElfProgramHeader))
        fail("the kernel's program headers are damaged");

    auto const* phs = reinterpret_cast<ElfProgramHeader const*>(file.data + h->programHeaderOffset);

    // Find the virtual range covered by all loadable segments.
    uint64_t low = ~0ull, high = 0;
    for (uint16_t i = 0; i < h->programHeaderCount; i++) {
        auto const& ph = phs[i];
        if (ph.type != PT_LOAD or ph.memorySize == 0)
            continue;
        if (ph.offset > file.size or ph.fileSize > file.size - ph.offset or ph.fileSize > ph.memorySize or
            ph.vaddr < KERNEL_MIN or ph.vaddr + ph.memorySize < ph.vaddr)
            fail("a kernel segment is outside the allowed range");
        if (ph.vaddr < low)
            low = ph.vaddr;
        if (ph.vaddr + ph.memorySize > high)
            high = ph.vaddr + ph.memorySize;
    }
    if (high == 0)
        fail("the kernel has nothing to load");

    low = alignDown(low, PAGE_SIZE);
    high = alignUp(high, PAGE_SIZE);
    if (h->entry < low or h->entry >= high)
        fail("the kernel's entry point is outside its image");

    // One physically contiguous block, so the kernel can compute physical
    // addresses of its own sections with a single offset.
    uint64_t size = high - low;
    uint64_t physical = allocatePages(size / PAGE_SIZE, Efi::SHOOL_KERNEL); // zeroed: .bss is ready

    for (uint16_t i = 0; i < h->programHeaderCount; i++) {
        auto const& ph = phs[i];
        if (ph.type != PT_LOAD or ph.memorySize == 0)
            continue;
        copyMemory(reinterpret_cast<void*>(physical + (ph.vaddr - low)), file.data + ph.offset, ph.fileSize);
    }

    return {physical, low, size, h->entry};
}

} // namespace Loader
