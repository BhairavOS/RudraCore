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

// An entry can start only if its kernel is in the signed list (an entry
// for a kernel this image doesn't ship, such as a previous kernel, is
// left out of the menu and skipped as a fallback).
bool usable(Entry const& e) { return isListed(e.kernel); }

// The menu: the default starts after `timeout` seconds; a key stops the
// countdown. 1-9 picks an entry, Enter the default, D adds diagnostics.
uint32_t chooseEntry(Config const& config, bool& diagnostic) {
    uint32_t shown[Config::MAX_ENTRIES];
    uint32_t count = 0;
    for (uint32_t i = 0; i < config.entryCount; i++) {
        if (usable(config.entries[i]))
            shown[count++] = i;
    }
    if (count == 0)
        fail("no entry's kernel is in the signed list");
    uint32_t choice = usable(config.entries[config.defaultEntry]) ? config.defaultEntry : shown[0];
    if (config.timeout == 0 or count < 2)
        return choice;

    print("\n");
    for (uint32_t n = 0; n < count; n++) {
        char number[] = {' ', ' ', ' ', ' ', static_cast<char>('1' + n), ' ', ' ', '\0'};
        print(number);
        print(config.entries[shown[n]].title);
        print(shown[n] == choice ? "   (default)\n" : "\n");
    }
    print("\n  1-");
    printNumber(count);
    print(" chooses, Enter starts the default, D adds diagnostics. The default starts in ");
    printNumber(config.timeout);
    print(" s.\n");
    bool stopped = false;
    for (uint64_t tick = 0; stopped or tick < config.timeout * 20ull; tick++) {
        Efi::Char16 key = pendingKey();
        if (key == 0) {
            bootServices->stall(50 * 1000);
            continue;
        }
        stopped = true;
        if (key == u'\r' or key == u'\n')
            break;
        if (key == u'd' or key == u'D') {
            diagnostic = true;
            print("  diagnostics on\n");
        } else if (key >= u'1' and key < static_cast<Efi::Char16>(u'1' + count)) {
            choice = shown[key - u'1'];
            break;
        }
    }
    return choice;
}

// Loads and starts one entry. Any failure fail()s, which (while the
// fallback is armed) comes back to efiMain to try the entry's fallback.
[[noreturn]] void bootEntry(Efi::File* root, Config const& config, Entry const& e, bool secureBoot, bool diagnostic) {
    print("\n  starting: ");
    print(e.title);
    print("\n");

    // The kernel.
    FileData kernelFile;
    if (not readFile(root, e.kernel, Efi::LOADER_DATA, kernelFile))
        fail("can't read the kernel");
    verifyFile(e.kernel, kernelFile);
    Kernel kernel = loadKernel(kernelFile);
    print("  kernel  ");
    print(e.kernel);
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
    info->mode = e.mode;
    info->hhdmOffset = Shool::HHDM_OFFSET;
    info->kernelPhysical = kernel.physical;
    info->kernelVirtual = kernel.virtualBase;
    info->acpiRsdp = findAcpiRsdp();

    // The programs that ship with the kernel. Loaded as kernel memory, so
    // they stay valid after the kernel reclaims loader memory. A missing
    // one stops this entry: the signed configuration names it.
    for (uint32_t i = 0; i < e.moduleCount; i++) {
        FileData module;
        if (not readFile(root, e.modules[i], Efi::SHOOL_KERNEL, module)) {
            print("  ");
            print(e.modules[i]);
            print(":\n");
            fail("a boot file named in shool.cfg is missing");
        }
        verifyFile(e.modules[i], module);
        checkModule(e.modules[i], module);
        auto& entry = info->modules[info->moduleCount++];
        entry.physical = reinterpret_cast<uint64_t>(module.data);
        entry.size = module.size;
        baseName(entry.name, sizeof(entry.name), e.modules[i]);
        if (diagnostic) {
            print("  module  ");
            print(e.modules[i]);
            printFileSize(module);
            print(", signed hash ok\n");
        }
    }
    if (not diagnostic) {
        print("  ");
        printNumber(e.moduleCount);
        print(" modules, every hash checked\n");
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

    disarmFallback(); // past here, a failure goes back to the firmware
    exitBootServices(*info);
    enterKernel(pml4, Shool::HHDM_OFFSET + stack + STACK_PAGES * PAGE_SIZE, kernel.entry, directMap(info));
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
    static bool diagnostic; // (static: kept across the fallback jump)
    diagnostic = diagnosticKey();

    // Secure Boot: when on, the firmware checked this loader's signature
    // before starting it, so the chain of trust starts in the firmware.
    bool secureBoot = globalFlag(u"SecureBoot") == 1 and globalFlag(u"SetupMode") != 1;
    print(secureBoot ? "  Secure Boot on: this loader's signature was checked by the firmware\n"
                     : "  Secure Boot off: nothing checked this loader\n");

    Efi::File* root = openBootVolume();
    loadBootManifest(root); // every file read below must be in its signed list
    Config const& config = readConfig(root);

    // Kernel fallback: when an entry fails to load (a missing or changed
    // file, a bad ELF, a protocol mismatch), fail() comes back here and the
    // entry's `fallback` is tried, each entry at most once.
    static uint32_t index;
    static bool tried[Config::MAX_ENTRIES];
    index = chooseEntry(config, diagnostic);
    if (__builtin_setjmp(fallbackPoint)) {
        int next = config.entries[index].fallback;
        for (uint32_t steps = 0; next >= 0 and (tried[next] or not usable(config.entries[next])) and
                                 steps < Config::MAX_ENTRIES;
             steps++)
            next = config.entries[next].fallback;
        if (next < 0 or tried[next] or not usable(config.entries[next]))
            fail("this entry failed and has no fallback left to try");
        print("  trying its fallback instead\n");
        index = static_cast<uint32_t>(next);
    }
    tried[index] = true;
    armFallback();
    Entry const& e = config.entries[index];
    bootEntry(root, config, e, secureBoot, diagnostic or e.diagnostic);
}
