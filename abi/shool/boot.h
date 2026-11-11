#pragma once

// The Shool boot protocol: how ShoolLoader🔱 hands over to RudraCore.
//
// Shared by the loader (built as a UEFI program, where `long` is 32 bits)
// and the kernel (where it's 64 bits), so only fixed-size types are used
// and the layout is checked below.
//
// Machine state when the kernel's ELF entry point runs:
//   - 64-bit mode, interrupts disabled, UEFI boot services exited
//   - rdi = SHOOL_MAGIC, rsi = pointer to BootInfo (in the direct map)
//   - paging on, with the loader's page tables:
//       * the kernel image at its ELF virtual addresses
//       * all memory regions mapped at HHDM_OFFSET + physical (direct map)
//         and at their physical address (identity map, for the hand-over)
//   - rsp on a 64 KiB stack in LoaderReclaimable memory
//   - EFER.NXE and CR0.WP enabled; PAT entries: 0 WB, 1 WT, 2 UC-, 3 UC,
//     4 WP, 5 WC (same layout as the Limine protocol)
//
// The page tables, stack and BootInfo are LoaderReclaimable: the kernel
// must copy what it needs and switch to its own page tables and stack
// before reusing that memory.

#include <stdint.h>

namespace Shool {

constexpr uint64_t MAGIC = 0x44414f4c4f4f4853; // "SHOOLOAD" in memory
constexpr uint32_t VERSION = 1;
constexpr uint64_t HHDM_OFFSET = 0xffff800000000000;

enum RegionType : uint32_t {
    REGION_USABLE = 0,
    REGION_RESERVED = 1,
    REGION_ACPI_RECLAIMABLE = 2,
    REGION_ACPI_NVS = 3,
    REGION_BAD_MEMORY = 4,
    REGION_LOADER_RECLAIMABLE = 5, // page tables, stack, BootInfo
    REGION_KERNEL = 6,             // the kernel image and modules
    REGION_FRAMEBUFFER = 7,
};

struct MemoryRegion {
    uint64_t base;
    uint64_t length;
    uint32_t type; // RegionType
    uint32_t reserved;
};

struct Framebuffer {
    uint64_t physical; // 0 if there's no framebuffer
    uint64_t width;
    uint64_t height;
    uint64_t pitch; // bytes per row
    uint16_t bpp;   // always 32 when present
    uint8_t redShift;
    uint8_t greenShift;
    uint8_t blueShift;
    uint8_t reserved[3];
};

// A file loaded next to the kernel (a program for the boot image).
struct Module {
    uint64_t physical;
    uint64_t size;
    char name[32]; // file name without directory or extension
};

struct BootInfo {
    static constexpr uint32_t MAX_REGIONS = 256;
    static constexpr uint32_t MAX_MODULES = 32;

    uint64_t magic; // MAGIC
    uint32_t version;
    uint32_t size; // sizeof(BootInfo)

    char loaderName[32];
    char loaderVersion[16];

    uint64_t hhdmOffset;
    uint64_t kernelPhysical; // where the kernel's lowest segment was loaded
    uint64_t kernelVirtual;  // its virtual address
    uint64_t acpiRsdp;       // physical address of the ACPI RSDP, or 0

    Framebuffer framebuffer;

    uint64_t regionCount;
    MemoryRegion regions[MAX_REGIONS];

    uint64_t moduleCount;
    Module modules[MAX_MODULES];
};

static_assert(sizeof(MemoryRegion) == 24);
static_assert(sizeof(Framebuffer) == 40);
static_assert(sizeof(Module) == 48);
static_assert(sizeof(BootInfo) == 16 + 48 + 32 + 40 + 8 + 24 * 256 + 8 + 48 * 32);

} // namespace Shool
