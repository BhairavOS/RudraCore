// Boot protocol glue: the only file that knows about bootloaders.
// UEFI boots go through ShoolLoader🔱 (the Shool protocol); legacy BIOS
// boots go through Limine.

#include "boot.h"

#include <limine.h>
#include <shool/boot.h>

namespace Rudracore::Boot {

namespace {

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
    if (not info.protocolSupported)
        return;

    copyString(info.loaderName, sizeof(info.loaderName), boot.loaderName);
    copyString(info.loaderVersion, sizeof(info.loaderVersion), boot.loaderVersion);
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
}

} // namespace

Info const& collect(u64 magic, u64 shoolInfo) {
    info = {};

    if (magic == Shool::MAGIC and shoolInfo) {
        auto const* boot = reinterpret_cast<Shool::BootInfo const*>(shoolInfo);
        if (boot->magic == Shool::MAGIC) {
            collectFromShool(*boot);
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

    return info;
}

} // namespace Rudracore::Boot
