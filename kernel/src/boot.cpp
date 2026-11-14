// Boot protocol glue: the only file that knows about bootloaders.
// UEFI boots go through ShoolLoader🔱 (the Shool protocol); legacy BIOS
// boots go through Limine.

#include "boot.h"

#include "log.h"

#include <limine.h>
#include <shool/boot.h>

namespace Rudracore::Boot {

namespace {

// The Shool protocol versions this kernel takes (ShoolLoader checks it).
[[gnu::used, gnu::section(".shool_kernel")]]
volatile Shool::KernelHeader const shoolHeader = {
    .magic = Shool::KERNEL_MAGIC,
    .check = ~Shool::KERNEL_MAGIC,
    .minVersion = Shool::VERSION,
    .maxVersion = Shool::VERSION,
};

// Requests are found by the bootloader by scanning between the start and end
// markers, so they must live in the .limine_requests* sections.

[[gnu::used, gnu::section(".limine_requests_start")]]
volatile u64 requestsStart[] = LIMINE_REQUESTS_START_MARKER;

[[gnu::used, gnu::section(".limine_requests")]]
volatile u64 baseRevision[] = LIMINE_BASE_REVISION(3);

[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_bootloader_info_request loaderRequest = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_framebuffer_request framebufferRequest = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_hhdm_request hhdmRequest = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_memmap_request memmapRequest = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_executable_address_request kernelAddressRequest = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_module_request moduleRequest = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
    .internal_module_count = 0,
    .internal_modules = nullptr,
};

[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_rsdp_request rsdpRequest = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_executable_cmdline_request cmdlineRequest = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

[[gnu::used, gnu::section(".limine_requests_end")]]
volatile u64 requestsEnd[] = LIMINE_REQUESTS_END_MARKER;

Info info;

void copyString(char* dest, usize size, char const* src) {
    usize i = 0;
    for (; src and src[i] and i + 1 < size; i++)
        dest[i] = src[i];
    dest[i] = '\0';
}

// "/boot/init.elf" -> "init"
void moduleName(char* dest, usize size, char const* path) {
    char const* start = path;
    for (char const* p = path; p and *p; p++) {
        if (*p == '/')
            start = p + 1;
    }
    usize i = 0;
    for (; start and start[i] and start[i] != '.' and i + 1 < size; i++)
        dest[i] = start[i];
    dest[i] = '\0';
}

RegionType regionTypeFromLimine(u64 type) {
    switch (type) {
    case LIMINE_MEMMAP_USABLE: return RegionType::Usable;
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE: return RegionType::AcpiReclaimable;
    case LIMINE_MEMMAP_ACPI_NVS: return RegionType::AcpiNvs;
    case LIMINE_MEMMAP_BAD_MEMORY: return RegionType::BadMemory;
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return RegionType::BootloaderReclaimable;
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: return RegionType::Kernel;
    case LIMINE_MEMMAP_FRAMEBUFFER: return RegionType::Framebuffer;
    case LIMINE_MEMMAP_RESERVED_MAPPED: return RegionType::ReservedMapped;
    default: return RegionType::Reserved;
    }
}

} // namespace

char const* regionTypeName(RegionType type) {
    switch (type) {
    case RegionType::Usable: return "usable";
    case RegionType::Reserved: return "reserved";
    case RegionType::AcpiReclaimable: return "acpi-reclaimable";
    case RegionType::AcpiNvs: return "acpi-nvs";
    case RegionType::BadMemory: return "bad";
    case RegionType::BootloaderReclaimable: return "bootloader";
    case RegionType::Kernel: return "kernel";
    case RegionType::Framebuffer: return "framebuffer";
    case RegionType::ReservedMapped: return "reserved-mapped";
    }
    return "?";
}

namespace {

RegionType regionTypeFromShool(u32 type) {
    switch (type) {
    case Shool::REGION_USABLE: return RegionType::Usable;
    case Shool::REGION_ACPI_RECLAIMABLE: return RegionType::AcpiReclaimable;
    case Shool::REGION_ACPI_NVS: return RegionType::AcpiNvs;
    case Shool::REGION_BAD_MEMORY: return RegionType::BadMemory;
    case Shool::REGION_LOADER_RECLAIMABLE: return RegionType::BootloaderReclaimable;
    case Shool::REGION_KERNEL: return RegionType::Kernel;
    case Shool::REGION_FRAMEBUFFER: return RegionType::Framebuffer;
    default: return RegionType::Reserved;
    }
}

void collectFromShool(Shool::BootInfo const& boot) {
    info.protocolSupported = boot.version == Shool::VERSION and boot.size == sizeof(Shool::BootInfo);
    if (not info.protocolSupported) {
        log("[boot] ShoolLoader speaks Shool protocol v", boot.version, ", this kernel v", Shool::VERSION);
        return;
    }

    copyString(info.loaderName, sizeof(info.loaderName), boot.loaderName);
    copyString(info.loaderVersion, sizeof(info.loaderVersion), boot.loaderVersion);
    info.verifiedBoot = boot.flags & Shool::FLAG_VERIFIED_BOOT;
    info.secureBoot = boot.flags & Shool::FLAG_SECURE_BOOT;
    info.diagnostic = boot.flags & Shool::FLAG_DIAGNOSTIC;
    info.mode = boot.mode <= Shool::MODE_RECOVERY ? boot.mode : Shool::MODE_SAFE; // (unknown: the careful choice)
    info.hhdmOffset = boot.hhdmOffset;
    info.kernelPhysicalBase = boot.kernelPhysical;
    info.kernelVirtualBase = boot.kernelVirtual;

    auto const& fb = boot.framebuffer;
    if (fb.physical and fb.bpp == 32) {
        info.hasFramebuffer = true;
        info.framebuffer = {
            .address = reinterpret_cast<u8*>(boot.hhdmOffset + fb.physical),
            .width = fb.width,
            .height = fb.height,
            .pitch = fb.pitch,
            .bpp = fb.bpp,
            .redShift = fb.redShift,
            .greenShift = fb.greenShift,
            .blueShift = fb.blueShift,
        };
    }

    for (u64 i = 0; i < boot.regionCount and i < Shool::BootInfo::MAX_REGIONS and
                    info.regionCount < Info::MAX_REGIONS;
         i++) {
        auto const& r = boot.regions[i];
        info.regions[info.regionCount++] = {r.base, r.length, regionTypeFromShool(r.type)};
    }

    for (u64 i = 0; i < boot.moduleCount and i < Shool::BootInfo::MAX_MODULES and
                    info.moduleCount < Info::MAX_MODULES;
         i++) {
        auto const& m = boot.modules[i];
        auto& module = info.modules[info.moduleCount++];
        module.data = reinterpret_cast<u8 const*>(boot.hhdmOffset + m.physical);
        module.size = m.size;
        copyString(module.name, sizeof(module.name), m.name);
    }
    info.acpiRsdp = boot.acpiRsdp;
}

// MARK: Validation

constexpr u64 PAGE = 4096;
constexpr u64 PHYSICAL_LIMIT = 1ull << 52; // x86-64's architectural maximum
constexpr u64 HIGHER_HALF = 0xffff800000000000;
constexpr u64 KERNEL_MIN = 0xffffffff80000000;

bool allocatable(RegionType t) { return t == RegionType::Usable or t == RegionType::BootloaderReclaimable; }
u64 alignUp(u64 v) { return (v + PAGE - 1) & ~(PAGE - 1); }
u64 alignDown(u64 v) { return v & ~(PAGE - 1); }

void refuse(char const* why) {
    log("[boot] unusable boot information: ", why);
    info.protocolSupported = false;
}

// (Takes the Info to work on, so selfTest can feed it a broken map.)
void validateRegions(Info& info) {
    // Drop impossible regions; page-align allocatable ones inward.
    usize kept = 0;
    for (usize i = 0; i < info.regionCount; i++) {
        Region r = info.regions[i];
        if (r.length == 0 or r.base >= PHYSICAL_LIMIT or r.length > PHYSICAL_LIMIT - r.base) {
            info.memoryFixes++;
            continue;
        }
        if (allocatable(r.type)) {
            u64 start = alignUp(r.base), end = alignDown(r.base + r.length);
            if (start >= end) {
                info.memoryFixes++;
                continue;
            }
            info.memoryFixes += start != r.base or end != r.base + r.length;
            r = {start, end - start, r.type};
        }
        info.regions[kept++] = r;
    }
    info.regionCount = kept;

    // Allocatable memory may overlap nothing: cut the overlap out of it (of
    // two allocatable regions, out of the later one). When the other region
    // sits in the middle, the part after it becomes a new region (or, with
    // the table full, the smaller side is dropped). Each cut shrinks a
    // region, so this ends.
    for (bool changed = true; changed;) {
        changed = false;
        for (usize i = 0; i < info.regionCount and not changed; i++) {
            Region& a = info.regions[i];
            if (not allocatable(a.type) or a.length == 0)
                continue;
            for (usize j = 0; j < info.regionCount; j++) {
                Region const& b = info.regions[j];
                if (j == i or b.length == 0 or (allocatable(b.type) and j > i))
                    continue;
                u64 aEnd = a.base + a.length, bEnd = b.base + b.length;
                if (b.base >= aEnd or a.base >= bEnd)
                    continue;
                u64 before = b.base > a.base ? alignDown(b.base) - a.base : 0;
                u64 afterStart = alignUp(bEnd);
                u64 after = aEnd > afterStart ? aEnd - afterStart : 0;
                if (before and after and info.regionCount < Info::MAX_REGIONS) {
                    info.regions[info.regionCount++] = {afterStart, after, a.type};
                    a.length = before;
                } else if (before >= after) {
                    a.length = before;
                } else {
                    a.base = afterStart;
                    a.length = after;
                }
                info.memoryFixes++;
                changed = true;
                break;
            }
        }
    }
    kept = 0;
    for (usize i = 0; i < info.regionCount; i++) {
        if (info.regions[i].length)
            info.regions[kept++] = info.regions[i];
    }
    info.regionCount = kept;
}

// True if [physical, physical + size) lies inside one Kernel region.
bool inKernelMemory(u64 physical, u64 size) {
    for (usize i = 0; i < info.regionCount; i++) {
        Region const& r = info.regions[i];
        if (r.type == RegionType::Kernel and physical >= r.base and physical - r.base <= r.length and
            size <= r.length - (physical - r.base))
            return true;
    }
    return false;
}

void validate() {
    if (info.hhdmOffset < HIGHER_HALF or info.hhdmOffset % PAGE)
        return refuse("the direct map isn't in the higher half");
    if (info.kernelVirtualBase < KERNEL_MIN or info.kernelPhysicalBase % PAGE)
        return refuse("the kernel isn't where it was linked to run");

    validateRegions(info);
    if (info.memoryFixes)
        log("[boot] memory map: repaired ", info.memoryFixes, " problem(s) (overlapping, unaligned or impossible regions)");
    bool usable = false;
    for (usize i = 0; i < info.regionCount; i++)
        usable = usable or info.regions[i].type == RegionType::Usable;
    if (not usable)
        return refuse("no usable memory");

    // Modules are read long after boot memory is reclaimed. Limine puts them
    // in bootloader-reclaimable memory: mark their pages kernel memory, so
    // the cut above keeps them out of what gets reclaimed.
    bool marked = false;
    for (usize i = 0; i < info.moduleCount; i++) {
        Module const& m = info.modules[i];
        u64 physical = reinterpret_cast<u64>(m.data) - info.hhdmOffset;
        if (reinterpret_cast<u64>(m.data) < info.hhdmOffset or inKernelMemory(physical, m.size) or
            info.regionCount == Info::MAX_REGIONS)
            continue;
        for (usize r = 0; r < info.regionCount; r++) {
            Region const& region = info.regions[r];
            if (region.type == RegionType::BootloaderReclaimable and physical >= region.base and
                physical - region.base <= region.length and m.size <= region.length - (physical - region.base)) {
                u64 start = alignDown(physical);
                info.regions[info.regionCount++] = {start, alignUp(physical + m.size) - start, RegionType::Kernel};
                marked = true;
                break;
            }
        }
    }
    if (marked) {
        usize fixes = info.memoryFixes;
        validateRegions(info);
        info.memoryFixes = fixes; // (expected with Limine, not a problem)
    }

    for (usize i = 0; i < info.moduleCount; i++) {
        Module const& m = info.modules[i];
        u64 physical = reinterpret_cast<u64>(m.data) - info.hhdmOffset;
        if (reinterpret_cast<u64>(m.data) < info.hhdmOffset or not inKernelMemory(physical, m.size)) {
            log("[boot] module ", m.name, " at ", Hex{physical}, " (", m.size, " bytes) isn't in kernel memory");
            for (usize r = 0; r < info.regionCount; r++)
                log("[boot]   ", Hex{info.regions[r].base}, " ", info.regions[r].length, " ",
                    regionTypeName(info.regions[r].type));
            return refuse("a module could be overwritten");
        }
    }

    if (info.hasFramebuffer) {
        auto const& fb = info.framebuffer;
        if (not fb.address or fb.bpp != 32 or fb.width == 0 or fb.height == 0 or fb.width > 16384 or
            fb.height > 16384 or fb.pitch < fb.width * 4 or fb.pitch > 65536) {
            log("[boot] the framebuffer doesn't make sense: serial output only");
            info.hasFramebuffer = false;
        }
    }
}

} // namespace

bool selfTest() {
    static Info t;
    t = {};
    constexpr auto U = RegionType::Usable, K = RegionType::Kernel, R = RegionType::Reserved,
                   B = RegionType::BootloaderReclaimable;
    Region const broken[] = {
        {0x1000, 0xF000, U},              // a kernel region in the middle: split in two
        {0x8000, 0x1000, K},
        {0x20010, 0x10000, U},            // unaligned: shrinks to whole pages
        {0x40000, 0x10000, U},            // the reserved region covers its start
        {0x3F800, 0x2000, R},
        {0x60000, 0x8000, U},             // two usable regions overlapping: the later one is cut
        {0x64000, 0x8000, B},
        {0x80000, 0, U},                  // empty
        {0xFFFFFFFFFFFFF000, 0x2000, U},  // wraps
        {1ull << 53, 0x1000, U},          // beyond any physical address
        {0x90000, 0x800, U},              // smaller than a page once aligned
    };
    for (Region const& r : broken)
        t.regions[t.regionCount++] = r;
    validateRegions(t);
    Region const expected[] = {
        {0x1000, 0x7000, U}, {0x8000, 0x1000, K}, {0x21000, 0xF000, U}, {0x42000, 0xE000, U},
        {0x3F800, 0x2000, R}, {0x60000, 0x8000, U}, {0x68000, 0x4000, B},
        {0x9000, 0x7000, U}, // the part of the first region after the kernel region
    };
    if (t.regionCount != sizeof(expected) / sizeof(expected[0]))
        return false;
    for (usize i = 0; i < t.regionCount; i++) {
        if (t.regions[i].base != expected[i].base or t.regions[i].length != expected[i].length or
            t.regions[i].type != expected[i].type)
            return false;
    }
    return true;
}

Info const& collect(u64 magic, u64 shoolInfo) {
    info = {};

    if (magic == Shool::MAGIC and shoolInfo) {
        auto const* boot = reinterpret_cast<Shool::BootInfo const*>(shoolInfo);
        if (boot->magic == Shool::MAGIC) {
            collectFromShool(*boot);
            if (info.protocolSupported)
                validate();
            return info;
        }
    }

    info.protocolSupported = LIMINE_BASE_REVISION_SUPPORTED(baseRevision);
    copyString(info.loaderName, sizeof(info.loaderName), "unknown");
    copyString(info.loaderVersion, sizeof(info.loaderVersion), "?");

    if (auto* res = loaderRequest.response) {
        copyString(info.loaderName, sizeof(info.loaderName), res->name);
        copyString(info.loaderVersion, sizeof(info.loaderVersion), res->version);
    }

    if (auto* res = framebufferRequest.response; res and res->framebuffer_count > 0) {
        auto* fb = res->framebuffers[0];
        if (fb->bpp == 32 and fb->memory_model == LIMINE_FRAMEBUFFER_RGB) {
            info.hasFramebuffer = true;
            info.framebuffer = {
                .address = static_cast<u8*>(fb->address),
                .width = fb->width,
                .height = fb->height,
                .pitch = fb->pitch,
                .bpp = fb->bpp,
                .redShift = fb->red_mask_shift,
                .greenShift = fb->green_mask_shift,
                .blueShift = fb->blue_mask_shift,
            };
        }
    }

    if (auto* res = hhdmRequest.response)
        info.hhdmOffset = res->offset;

    if (auto* res = kernelAddressRequest.response) {
        info.kernelPhysicalBase = res->physical_base;
        info.kernelVirtualBase = res->virtual_base;
    }

    if (auto* res = memmapRequest.response) {
        for (u64 i = 0; i < res->entry_count and info.regionCount < Info::MAX_REGIONS; i++) {
            auto* entry = res->entries[i];
            info.regions[info.regionCount++] = {
                .base = entry->base,
                .length = entry->length,
                .type = regionTypeFromLimine(entry->type),
            };
        }
    }

    if (auto* res = moduleRequest.response) {
        for (u64 i = 0; i < res->module_count and info.moduleCount < Info::MAX_MODULES; i++) {
            auto* file = res->modules[i];
            auto& module = info.modules[info.moduleCount++];
            module.data = static_cast<u8 const*>(file->address);
            module.size = file->size;
            moduleName(module.name, sizeof(module.name), file->path);
        }
    }

    if (auto* res = rsdpRequest.response) {
        // Physical or direct-mapped, depending on the protocol revision.
        u64 address = reinterpret_cast<u64>(res->address);
        info.acpiRsdp = address >= info.hhdmOffset ? address - info.hhdmOffset : address;
    }

    // limine.conf's "cmdline: mode=safe" (or mode=recovery) picks the mode.
    if (auto* res = cmdlineRequest.response; res and res->cmdline) {
        char const* c = res->cmdline;
        for (usize i = 0; c[i] and i < 256; i++) {
            auto is = [&](char const* word) {
                usize n = 0;
                while (word[n] and c[i + n] == word[n])
                    n++;
                return not word[n] and (c[i + n] == '\0' or c[i + n] == ' ');
            };
            if ((i == 0 or c[i - 1] == ' ') and is("mode=safe"))
                info.mode = Shool::MODE_SAFE;
            else if ((i == 0 or c[i - 1] == ' ') and is("mode=recovery"))
                info.mode = Shool::MODE_RECOVERY;
        }
    }

    if (info.protocolSupported)
        validate();
    return info;
}

} // namespace Rudracore::Boot
