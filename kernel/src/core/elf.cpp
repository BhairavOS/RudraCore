#include "core/elf.h"

#include "mem/memory.h"
#include "mem/pmm.h"

extern "C" void* memcpy(void* dest, void const* src, usize n);

namespace Rudracore::Elf {

namespace {

struct Header {
    u8 ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u64 entry;
    u64 programHeaderOffset;
    u64 sectionHeaderOffset;
    u32 flags;
    u16 headerSize;
    u16 programHeaderSize;
    u16 programHeaderCount;
    u16 sectionHeaderSize;
    u16 sectionHeaderCount;
    u16 sectionNameIndex;
};

struct ProgramHeader {
    u32 type;
    u32 flags;
    u64 offset;
    u64 vaddr;
    u64 paddr;
    u64 fileSize;
    u64 memorySize;
    u64 align;
};

constexpr u8 CLASS_64 = 2;
constexpr u8 LITTLE_ENDIAN = 1;
constexpr u16 TYPE_EXECUTABLE = 2;
constexpr u16 MACHINE_X86_64 = 62;
constexpr u32 SEGMENT_LOAD = 1;
constexpr u32 FLAG_EXEC = 1;
constexpr u32 FLAG_WRITE = 2;

// Lowest address a program may use, so null pointers always fault.
constexpr u64 USER_MIN = 0x10000;

bool validHeader(u8 const* data, usize size, char const*& error) {
    if (size < sizeof(Header)) {
        error = "file too small";
        return false;
    }

    auto const* h = reinterpret_cast<Header const*>(data);
    if (h->ident[0] != 0x7F or h->ident[1] != 'E' or h->ident[2] != 'L' or h->ident[3] != 'F') {
        error = "not an ELF file";
        return false;
    }
    if (h->ident[4] != CLASS_64 or h->ident[5] != LITTLE_ENDIAN or
        h->machine != MACHINE_X86_64 or h->type != TYPE_EXECUTABLE) {
        error = "not a 64-bit x86_64 executable";
        return false;
    }
    if (h->programHeaderSize != sizeof(ProgramHeader) or
        h->programHeaderOffset > size or
        h->programHeaderCount > (size - h->programHeaderOffset) / sizeof(ProgramHeader)) {
        error = "bad program header table";
        return false;
    }
    return true;
}

bool loadSegment(Vmm::Space& space, u8 const* data, usize size, ProgramHeader const& ph, char const*& error) {
    u64 end = ph.vaddr + ph.memorySize;
    if (ph.fileSize > ph.memorySize or ph.offset > size or ph.fileSize > size - ph.offset or
        end < ph.vaddr or ph.vaddr < USER_MIN or end > USER_STACK_TOP - USER_STACK_SIZE) {
        error = "segment outside of the allowed range";
        return false;
    }

    if ((ph.flags & FLAG_WRITE) and (ph.flags & FLAG_EXEC)) {
        error = "a segment is both writable and executable";
        return false;
    }

    u32 access = Vmm::USER | Vmm::OWNED;
    if (ph.flags & FLAG_WRITE)
        access |= Vmm::WRITE;
    if (ph.flags & FLAG_EXEC)
        access |= Vmm::EXEC;

    u64 fileEnd = ph.vaddr + ph.fileSize;
    for (u64 page = Mem::alignDown(ph.vaddr, Mem::PAGE_SIZE); page < end; page += Mem::PAGE_SIZE) {
        u64 physical;
        if (not space.translate(page, physical)) {
            physical = Pmm::allocZeroed();
            if (not physical or not space.map(page, physical, Mem::PAGE_SIZE, access)) {
                if (physical)
                    Pmm::free(physical);
                error = OUT_OF_MEMORY;
                return false;
            }
        }

        // Copy the part of the file data that falls in this page; the rest
        // stays zero (that's how .bss gets cleared).
        u64 copyStart = max(page, ph.vaddr);
        u64 copyEnd = min(page + Mem::PAGE_SIZE, fileEnd);
        if (copyStart < copyEnd) {
            memcpy(Mem::toVirtual<u8>(physical) + (copyStart - page),
                   data + ph.offset + (copyStart - ph.vaddr),
                   copyEnd - copyStart);
        }
    }
    return true;
}

bool mapStack(Vmm::Space& space) {
    for (u64 page = USER_STACK_TOP - USER_STACK_SIZE; page < USER_STACK_TOP; page += Mem::PAGE_SIZE) {
        u64 physical = Pmm::allocZeroed();
        if (not physical)
            return false;
        if (not space.map(page, physical, Mem::PAGE_SIZE, Vmm::USER | Vmm::OWNED | Vmm::WRITE)) {
            Pmm::free(physical);
            return false;
        }
    }
    return true;
}

} // namespace

bool load(u8 const* data, usize size, Program& out, char const*& error) {
    if (not validHeader(data, size, error))
        return false;

    auto const* header = reinterpret_cast<Header const*>(data);
    if (header->entry < USER_MIN or header->entry >= Vmm::USER_END) {
        error = "entry point outside of user memory";
        return false;
    }

    Vmm::Space* space = Vmm::createUserSpace();
    if (not space) {
        error = OUT_OF_MEMORY;
        return false;
    }

    auto const* headers = reinterpret_cast<ProgramHeader const*>(data + header->programHeaderOffset);

    // Programs can come from anywhere (installed apps): refuse any that
    // asks for an unreasonable amount of memory before allocating it.
    constexpr u64 MAX_PROGRAM_MEMORY = 256ull * 1024 * 1024;
    u64 total = 0;
    for (u16 i = 0; i < header->programHeaderCount; i++) {
        if (headers[i].type != SEGMENT_LOAD)
            continue;
        if (headers[i].memorySize > MAX_PROGRAM_MEMORY or total + headers[i].memorySize > MAX_PROGRAM_MEMORY) {
            error = "the program asks for too much memory";
            Vmm::destroyUserSpace(space);
            return false;
        }
        total += headers[i].memorySize;
    }

    // All it will take: segments (each may straddle one more page), page
    // tables, its stack, and the task's kernel stack and state.
    u64 pages = total / Mem::PAGE_SIZE + 2 * header->programHeaderCount + USER_STACK_SIZE / Mem::PAGE_SIZE + 16;
    if (not Pmm::userCanUse(pages)) {
        error = OUT_OF_MEMORY;
        Vmm::destroyUserSpace(space);
        return false;
    }

    bool loadedAny = false;
    for (u16 i = 0; i < header->programHeaderCount; i++) {
        if (headers[i].type != SEGMENT_LOAD or headers[i].memorySize == 0)
            continue;
        if (not loadSegment(*space, data, size, headers[i], error)) {
            Vmm::destroyUserSpace(space);
            return false;
        }
        loadedAny = true;
    }

    if (not loadedAny) {
        error = "no loadable segments";
        Vmm::destroyUserSpace(space);
        return false;
    }

    if (not mapStack(*space)) {
        error = OUT_OF_MEMORY;
        Vmm::destroyUserSpace(space);
        return false;
    }

    out = {space, header->entry, USER_STACK_TOP};
    return true;
}

} // namespace Rudracore::Elf
