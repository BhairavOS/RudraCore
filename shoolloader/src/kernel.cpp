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
constexpr uint32_t PF_X = 1;
constexpr uint16_t MAX_PROGRAM_HEADERS = 64;

// The kernel must live in the top 2 GiB (it's built with -mcmodel=kernel).
constexpr uint64_t KERNEL_MIN = 0xffffffff80000000;
constexpr uint64_t MAX_KERNEL_SIZE = 64ull * 1024 * 1024;

// The header checks the kernel and every .elf module share: a 64-bit,
// little-endian, current-version x86_64 executable whose program headers
// and loadable segments lie inside the file. nullptr if fine, else why not.
char const* checkHeader(FileData const& file) {
    auto const* h = reinterpret_cast<ElfHeader const*>(file.data);
    if (file.size < sizeof(ElfHeader) or h->ident[0] != 0x7F or h->ident[1] != 'E' or h->ident[2] != 'L' or
        h->ident[3] != 'F')
        return "not an ELF file";
    if (h->ident[4] != 2 or h->ident[5] != 1 or h->ident[6] != 1 or h->version != 1 or h->machine != 62 or
        h->type != 2 or h->headerSize != sizeof(ElfHeader))
        return "not a 64-bit little-endian x86_64 ELF executable";
    if (h->programHeaderSize != sizeof(ElfProgramHeader) or h->programHeaderCount == 0 or
        h->programHeaderCount > MAX_PROGRAM_HEADERS or h->programHeaderOffset > file.size or
        h->programHeaderCount > (file.size - h->programHeaderOffset) / sizeof(ElfProgramHeader) or
        h->programHeaderOffset % 8)
        return "damaged program headers";
    auto const* phs = reinterpret_cast<ElfProgramHeader const*>(file.data + h->programHeaderOffset);
    for (uint16_t i = 0; i < h->programHeaderCount; i++) {
        auto const& ph = phs[i];
        if (ph.type == PT_LOAD and (ph.offset > file.size or ph.fileSize > file.size - ph.offset or
                                    ph.fileSize > ph.memorySize or ph.vaddr + ph.memorySize < ph.vaddr or
                                    (ph.vaddr - ph.offset) % PAGE_SIZE))
            return "a segment lies outside the file, wraps, or isn't page-congruent";
    }
    return nullptr;
}

// The kernel must say it takes this loader's protocol version.
void checkProtocol(FileData const& file) {
    Shool::KernelHeader const* header = nullptr;
    for (uint64_t at = 0; at + sizeof(Shool::KernelHeader) <= file.size; at += 8) {
        auto const* h = reinterpret_cast<Shool::KernelHeader const*>(file.data + at);
        if (h->magic == Shool::KERNEL_MAGIC and h->check == ~Shool::KERNEL_MAGIC) {
            header = h;
            break;
        }
    }
    if (not header)
        fail("the kernel doesn't say which Shool protocol it speaks (no KernelHeader)");
    if (Shool::VERSION < header->minVersion or Shool::VERSION > header->maxVersion) {
        print("  this loader speaks Shool protocol v");
        printNumber(Shool::VERSION);
        print("; the kernel takes v");
        printNumber(header->minVersion);
        print(" to v");
        printNumber(header->maxVersion);
        print("\n");
        fail("the kernel and the loader don't speak the same boot protocol version");
    }
}

} // namespace

void checkModule(char const* path, FileData const& file) {
    uint64_t n = 0;
    while (path[n])
        n++;
    if (n < 4 or path[n - 4] != '.' or path[n - 3] != 'e' or path[n - 2] != 'l' or path[n - 1] != 'f')
        return; // data, not a program
    if (char const* why = checkHeader(file)) {
        print("  ");
        print(path);
        print(": ");
        print(why);
        print("\n");
        fail("a boot program isn't a valid ELF executable");
    }
}

Kernel loadKernel(FileData const& file) {
    if (char const* why = checkHeader(file)) {
        print("  kernel: ");
        print(why);
        print("\n");
        fail("the kernel isn't a valid 64-bit x86_64 ELF executable");
    }
    checkProtocol(file);
    auto const* h = reinterpret_cast<ElfHeader const*>(file.data);
    auto const* phs = reinterpret_cast<ElfProgramHeader const*>(file.data + h->programHeaderOffset);

    // Find the virtual range covered by all loadable segments; they must be
    // in the top 2 GiB, not overlap, and the entry point must be in an
    // executable one.
    uint64_t low = ~0ull, high = 0;
    bool entryOk = false;
    for (uint16_t i = 0; i < h->programHeaderCount; i++) {
        auto const& ph = phs[i];
        if (ph.type != PT_LOAD or ph.memorySize == 0)
            continue;
        if (ph.vaddr < KERNEL_MIN)
            fail("a kernel segment is outside the top 2 GiB");
        for (uint16_t j = 0; j < i; j++) {
            auto const& other = phs[j];
            if (other.type == PT_LOAD and other.memorySize and
                alignDown(ph.vaddr, PAGE_SIZE) < alignUp(other.vaddr + other.memorySize, PAGE_SIZE) and
                alignDown(other.vaddr, PAGE_SIZE) < alignUp(ph.vaddr + ph.memorySize, PAGE_SIZE))
                fail("two kernel segments overlap (or share a page)");
        }
        entryOk = entryOk or ((ph.flags & PF_X) and h->entry >= ph.vaddr and h->entry < ph.vaddr + ph.memorySize);
        if (ph.vaddr < low)
            low = ph.vaddr;
        if (ph.vaddr + ph.memorySize > high)
            high = ph.vaddr + ph.memorySize;
    }
    if (high == 0)
        fail("the kernel has nothing to load");
    if (not entryOk)
        fail("the kernel's entry point isn't in an executable segment");

    low = alignDown(low, PAGE_SIZE);
    high = alignUp(high, PAGE_SIZE);
    if (high - low > MAX_KERNEL_SIZE)
        fail("the kernel image is bigger than 64 MiB");

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
