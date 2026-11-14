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
    static constexpr usize MAX_MODULES = 32;

    bool protocolSupported; // false: unusable (an unknown protocol, or it failed validation)
    char loaderName[64];
    char loaderVersion[64];

    bool verifiedBoot; // every boot file came from a signed list (ShoolLoader)
    bool secureBoot;   // UEFI Secure Boot was on
    bool diagnostic;   // diagnostic boot: log everything about the boot
    u64 mode;          // Abi::BOOT_NORMAL/SAFE/RECOVERY (ShoolLoader's entry, or Limine's "mode=" cmdline)
    usize memoryFixes; // memory map problems validation repaired

    bool hasFramebuffer;
    Framebuffer framebuffer;

    u64 hhdmOffset; // virtual = physical + hhdmOffset
    u64 kernelPhysicalBase;
    u64 kernelVirtualBase;

    Region regions[MAX_REGIONS];
    usize regionCount;

    Module modules[MAX_MODULES];
    usize moduleCount;

    u64 acpiRsdp; // physical address of the ACPI RSDP, or 0
};

// Reads the boot information from whichever bootloader started the kernel.
// ShoolLoader passes (Shool::MAGIC, BootInfo*) in rdi/rsi; anything else
// means Limine, which answers the requests embedded in the kernel image.
//
// Then validates it, trusting neither loader: regions that wrap or are
// empty are dropped, usable ones are page-aligned, and usable memory that
// overlaps anything else is cut away (so the kernel never hands out memory
// that holds itself, a module or firmware data). Modules must lie inside
// kernel memory and the framebuffer must make sense. Problems it can't
// repair clear protocolSupported (and are logged).
Info const& collect(u64 magic, u64 shoolInfo);

// Runs the memory map repair on a deliberately broken map and checks the
// result. True if it came out right.
bool selfTest();

} // namespace Rudracore::Boot
