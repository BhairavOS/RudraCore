// Limine boot protocol glue. This is the only file that knows about Limine.

#include "boot.h"

#include <limine.h>

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

[[gnu::used, gnu::section(".limine_requests_end")]]
volatile u64 requestsEnd[] = LIMINE_REQUESTS_END_MARKER;

} // namespace

Info collect() {
    Info info{};
    info.protocolSupported = LIMINE_BASE_REVISION_SUPPORTED(baseRevision);
    info.loaderName = "unknown";
    info.loaderVersion = "?";

    if (auto* res = loaderRequest.response) {
        info.loaderName = res->name;
        info.loaderVersion = res->version;
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

    if (auto* res = memmapRequest.response) {
        info.memmapEntries = res->entry_count;
        for (u64 i = 0; i < res->entry_count; i++) {
            auto* entry = res->entries[i];
            if (entry->type == LIMINE_MEMMAP_USABLE)
                info.usableMemory += entry->length;
            else if (entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE)
                info.reclaimableMemory += entry->length;
        }
    }

    return info;
}

} // namespace Rudracore::Boot
