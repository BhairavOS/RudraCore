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

// Everything the kernel needs from the bootloader, in our own types, so the
// rest of the kernel never depends on the boot protocol (Limine today,
// ShoolLoader later).
struct Info {
    bool protocolSupported;
    char const* loaderName;
    char const* loaderVersion;

    bool hasFramebuffer;
    Framebuffer framebuffer;

    u64 hhdmOffset;
    u64 memmapEntries;
    u64 usableMemory;
    u64 reclaimableMemory;
};

Info collect();

} // namespace Rudracore::Boot
