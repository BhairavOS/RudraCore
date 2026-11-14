// ShoolLoader🔱 - Trident of System Awakening

#include "loader.h"

using namespace Loader;

namespace {

constexpr uint64_t STACK_PAGES = 16; // 64 KiB

void copyName(char* dest, uint64_t size, char const* src) {
    uint64_t i = 0;
    for (; src[i] and i + 1 < size; i++)
        dest[i] = src[i];
    dest[i] = '\0';
}

uint64_t findAcpiRsdp() {
    uint64_t acpi1 = 0;
    for (uint64_t i = 0; i < system->numberOfTableEntries; i++) {
        auto const& table = system->configurationTable[i];
        if (table.vendorGuid == Efi::ACPI_20_TABLE)
            return reinterpret_cast<uint64_t>(table.vendorTable);
        if (table.vendorGuid == Efi::ACPI_10_TABLE)
            acpi1 = reinterpret_cast<uint64_t>(table.vendorTable);
    }
    return acpi1;
}

template <typename T>
T* directMap(T* physical) {
    return reinterpret_cast<T*>(Shool::HHDM_OFFSET + reinterpret_cast<uint64_t>(physical));
}

} // namespace

extern "C" Efi::Status efiMain(Efi::Handle image, Efi::SystemTable* systemTable) {
    system = systemTable;
    bootServices = systemTable->bootServices;
    imageHandle = image;

    bootServices->setWatchdogTimer(0, 0, 0, nullptr); // no reset if loading takes a while
    system->conOut->clearScreen(system->conOut);
    print("ShoolLoader ");
    print(VERSION);
    print(" - Trident of System Awakening\n\n");

    Efi::File* root = openBootVolume();
    loadBootManifest(root); // every file read below must be in its signed list
    Config config = readConfig(root);

    // The kernel.
    FileData kernelFile;
    if (not readFile(root, config.kernel, Efi::LOADER_DATA, kernelFile))
        fail("can't read the kernel (check 'kernel =' in /boot/shool.cfg)");
    verifyFile(config.kernel, kernelFile);
    Kernel kernel = loadKernel(kernelFile);
    print("  kernel  ");
    print(config.kernel);
    print("  ");
    printSize(kernel.size);
    print("\n");

    auto* info = reinterpret_cast<Shool::BootInfo*>(
        allocatePages(alignUp(sizeof(Shool::BootInfo), PAGE_SIZE) / PAGE_SIZE, Efi::SHOOL_RECLAIMABLE));
    info->magic = Shool::MAGIC;
    info->version = Shool::VERSION;
    info->size = sizeof(Shool::BootInfo);
    copyName(info->loaderName, sizeof(info->loaderName), NAME);
    copyName(info->loaderVersion, sizeof(info->loaderVersion), VERSION);
    info->hhdmOffset = Shool::HHDM_OFFSET;
    info->kernelPhysical = kernel.physical;
    info->kernelVirtual = kernel.virtualBase;
    info->acpiRsdp = findAcpiRsdp();

    // The programs that ship with the kernel. Loaded as kernel memory, so
    // they stay valid after the kernel reclaims loader memory.
    for (uint32_t i = 0; i < config.moduleCount; i++) {
        FileData module;
        if (not readFile(root, config.modules[i], Efi::SHOOL_KERNEL, module)) {
            print("  missing ");
            print(config.modules[i]);
            print(", skipping it\n");
            continue;
        }
        verifyFile(config.modules[i], module);
        auto& entry = info->modules[info->moduleCount++];
        entry.physical = reinterpret_cast<uint64_t>(module.data);
        entry.size = module.size;
        baseName(entry.name, sizeof(entry.name), config.modules[i]);
        print("  module  ");
        print(config.modules[i]);
        print("  ");
        printSize(module.size);
        print("\n");
    }

    print("\nawakening RudraCore...\n");
    setupGraphics(config, info->framebuffer);
    drawTrident(info->framebuffer);
    bootServices->stall(400 * 1000); // let the trident be seen

    uint64_t stack = allocatePages(STACK_PAGES, Efi::SHOOL_RECLAIMABLE);
    uint64_t pml4 = buildPageTables(kernel, info->framebuffer);

    exitBootServices(*info);
    enterKernel(pml4, Shool::HHDM_OFFSET + stack + STACK_PAGES * PAGE_SIZE, kernel.entry, directMap(info));
}
