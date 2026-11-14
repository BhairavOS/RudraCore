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

// A one-byte UEFI global variable ("SecureBoot", "SetupMode"), or -1.
int globalFlag(Efi::Char16 const* name) {
    uint8_t value = 0;
    uint64_t size = 1;
    if (Efi::failed(system->runtimeServices->getVariable(name, &Efi::GLOBAL_VARIABLE, nullptr, &size, &value)) or
        size != 1)
        return -1;
    return value;
}

// D (held while booting, or pressed now) asks for a diagnostic boot.
bool diagnosticKey() {
    for (int i = 0; i < 64; i++) {
        Efi::Char16 key = pendingKey();
        if (key == 0)
            return false;
        if (key == u'd' or key == u'D')
            return true;
    }
    return false;
}

void printFileSize(FileData const& file) {
    print("  ");
    printSize(file.size);
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
    bool diagnostic = diagnosticKey();

    // Secure Boot: when on, the firmware checked this loader's signature
    // before starting it, so the chain of trust starts in the firmware.
    bool secureBoot = globalFlag(u"SecureBoot") == 1 and globalFlag(u"SetupMode") != 1;
    print(secureBoot ? "  Secure Boot on: this loader's signature was checked by the firmware\n"
                     : "  Secure Boot off: nothing checked this loader\n");

    Efi::File* root = openBootVolume();
    loadBootManifest(root); // every file read below must be in its signed list
    Config config = readConfig(root);
    diagnostic = diagnostic or config.diagnostic;
    if (diagnostic)
        print("  diagnostic boot: every step is shown, and the loader waits before handing over\n");

    // The kernel.
    FileData kernelFile;
    if (not readFile(root, config.kernel, Efi::LOADER_DATA, kernelFile))
        fail("can't read the kernel (check 'kernel =' in /boot/shool.cfg)");
    verifyFile(config.kernel, kernelFile);
    Kernel kernel = loadKernel(kernelFile);
    print("  kernel  ");
    print(config.kernel);
    printFileSize(kernelFile);
    print(diagnostic ? ", signed hash ok, ELF ok, protocol ok\n" : "\n");

    auto* info = reinterpret_cast<Shool::BootInfo*>(
        allocatePages(alignUp(sizeof(Shool::BootInfo), PAGE_SIZE) / PAGE_SIZE, Efi::SHOOL_RECLAIMABLE));
    info->magic = Shool::MAGIC;
    info->version = Shool::VERSION;
    info->size = sizeof(Shool::BootInfo);
    copyName(info->loaderName, sizeof(info->loaderName), NAME);
    copyName(info->loaderVersion, sizeof(info->loaderVersion), VERSION);
    info->flags = Shool::FLAG_VERIFIED_BOOT | (secureBoot ? Shool::FLAG_SECURE_BOOT : 0) |
                  (diagnostic ? Shool::FLAG_DIAGNOSTIC : 0);
    info->hhdmOffset = Shool::HHDM_OFFSET;
    info->kernelPhysical = kernel.physical;
    info->kernelVirtual = kernel.virtualBase;
    info->acpiRsdp = findAcpiRsdp();

    // The programs that ship with the kernel. Loaded as kernel memory, so
    // they stay valid after the kernel reclaims loader memory. A missing
    // one stops the boot: the signed configuration names it.
    for (uint32_t i = 0; i < config.moduleCount; i++) {
        FileData module;
        if (not readFile(root, config.modules[i], Efi::SHOOL_KERNEL, module)) {
            print("  ");
            print(config.modules[i]);
            print(":\n");
            fail("a boot file named in shool.cfg is missing");
        }
        verifyFile(config.modules[i], module);
        checkModule(config.modules[i], module);
        auto& entry = info->modules[info->moduleCount++];
        entry.physical = reinterpret_cast<uint64_t>(module.data);
        entry.size = module.size;
        baseName(entry.name, sizeof(entry.name), config.modules[i]);
        print("  module  ");
        print(config.modules[i]);
        printFileSize(module);
        print(diagnostic ? ", signed hash ok\n" : "\n");
    }

    print("\nawakening RudraCore...\n");
    setupGraphics(config, info->framebuffer);
    if (diagnostic) {
        auto const& fb = info->framebuffer;
        print("  screen ");
        printNumber(fb.width);
        print("x");
        printNumber(fb.height);
        print(fb.physical ? "\n" : " (no framebuffer)\n");
    }

    uint64_t stack = allocatePages(STACK_PAGES, Efi::SHOOL_RECLAIMABLE);
    uint64_t pml4 = buildPageTables(kernel, info->framebuffer);
    checkHandover(pml4, kernel, stack, STACK_PAGES * PAGE_SIZE, info);

    if (diagnostic) {
        print("  hand-over checked: page tables, stack, boot info, CPU (NX)\n"
              "\nPress a key to start the kernel (it continues by itself in 60 seconds).\n");
        waitForKey(60);
    } else {
        drawTrident(info->framebuffer);
        bootServices->stall(400 * 1000); // let the trident be seen
    }

    exitBootServices(*info);
    enterKernel(pml4, Shool::HHDM_OFFSET + stack + STACK_PAGES * PAGE_SIZE, kernel.entry, directMap(info));
}
