#pragma once

#include "types.h"

namespace Rudracore::Boot {

struct Framebuffer {
    u8* address;
    u64 width;
    u64 height;
    u64 pitch; // bytes per row
    u16 bpp;
    u8 redShift;
    u8 greenShift;
    u8 blueShift;
};

enum class RegionType : u8 {
    Usable,
    Reserved,
    AcpiReclaimable,
    AcpiNvs,
    BadMemory,
    BootloaderReclaimable, // bootloader page tables, stack and boot info
    Kernel,                // the kernel image (and boot modules)
    Framebuffer,
    ReservedMapped,
};

char const* regionTypeName(RegionType type);

struct Region {
    u64 base;
    u64 length;
    RegionType type;
};

// A file the bootloader loaded next to the kernel, such as the init program.
// Its memory is a Kernel region, so it stays valid after boot memory is
// reclaimed.
struct Module {
    u8 const* data;
    u64 size;
    char name[32]; // file name without directory or extension
};

// Everything the kernel needs from the bootloader, copied into the kernel's
// own memory. Once collected, nothing in the bootloader's memory is used
// again, so it can be reclaimed. The rest of the kernel never depends on the
// boot protocol (Limine today, ShoolLoader later).
struct Info {
    static constexpr usize MAX_REGIONS = 256;
    static constexpr usize MAX_MODULES = 16;

    bool protocolSupported;
    char loaderName[64];
    char loaderVersion[64];

    bool hasFramebuffer;
    Framebuffer framebuffer;

    u64 hhdmOffset; // virtual = physical + hhdmOffset
    u64 kernelPhysicalBase;
    u64 kernelVirtualBase;

    Region regions[MAX_REGIONS];
    usize regionCount;

    Module modules[MAX_MODULES];
    usize moduleCount;
};

// Reads the boot information from whichever bootloader started the kernel.
// ShoolLoader passes (Shool::MAGIC, BootInfo*) in rdi/rsi; anything else
// means Limine, which answers the requests embedded in the kernel image.
Info const& collect(u64 magic, u64 shoolInfo);

} // namespace Rudracore::Boot
