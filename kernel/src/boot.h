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

// Everything the kernel needs from the bootloader, copied into the kernel's
// own memory. Once collected, nothing in the bootloader's memory is used
// again, so it can be reclaimed. The rest of the kernel never depends on the
// boot protocol (Limine today, ShoolLoader later).
struct Info {
    static constexpr usize MAX_REGIONS = 256;

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
};

Info const& collect();

} // namespace Rudracore::Boot
