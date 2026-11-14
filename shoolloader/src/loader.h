#pragma once

// ShoolLoader🔱 - Trident of System Awakening
//
// The Bhairav OS UEFI bootloader: reads /boot/shool.cfg, loads RudraCore
// and the boot programs, sets up the screen and the first page tables, and
// hands over with the Shool boot protocol (abi/shool/boot.h).

#include "efi.h"

#include <shool/boot.h>

namespace Loader {

constexpr char const* NAME = "ShoolLoader";
constexpr char const* VERSION = "0.1.0";

constexpr uint64_t PAGE_SIZE = 4096;

inline uint64_t alignUp(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
inline uint64_t alignDown(uint64_t v, uint64_t a) { return v & ~(a - 1); }

extern Efi::SystemTable* system;
extern Efi::BootServices* bootServices;
extern Efi::Handle imageHandle;

// MARK: Console (only until boot services are exited)

void print(char const* text);
void printNumber(uint64_t value);
void printSize(uint64_t bytes);

// Shows the error, waits so it can be read, then stops.
[[noreturn]] void fail(char const* what, Efi::Status status = 0);

// MARK: Memory

void copyMemory(void* dest, void const* src, uint64_t size);
void zeroMemory(void* dest, uint64_t size);

// Zeroed pages of the given type, or fail(). UEFI identity-maps memory, so
// the physical address is also usable as a pointer.
uint64_t allocatePages(uint64_t count, Efi::MemoryType type);

// MARK: Files and configuration

struct FileData {
    uint8_t* data;
    uint64_t size;
};

// The volume ShoolLoader itself was loaded from.
Efi::File* openBootVolume();

// Reads a whole file ("/boot/x.elf") into fresh pages of `type`.
bool readFile(Efi::File* root, char const* path, Efi::MemoryType type, FileData& out);

struct Config {
    static constexpr uint32_t MAX_PATH = 96;

    char kernel[MAX_PATH];
    char modules[Shool::BootInfo::MAX_MODULES][MAX_PATH];
    uint32_t moduleCount;
    uint32_t width, height; // preferred resolution, 0 = automatic
};

// Reads /boot/shool.cfg (defaults when missing):
//   kernel = /boot/rudracore.elf
//   module = /boot/init.elf        (repeatable)
//   resolution = 1280x800
Config readConfig(Efi::File* root);

// "/boot/init.elf" -> "init"
void baseName(char* dest, uint64_t size, char const* path);

// MARK: Verified boot (verify.cpp)

// Reads and checks the signed list of boot files, and refuses an older boot
// image than one that already booted. fail()s otherwise.
void loadBootManifest(Efi::File* root);

// fail()s unless the file is in the signed list with this content.
void verifyFile(char const* path, FileData const& file);

// MARK: Kernel

struct Kernel {
    uint64_t physical;    // where the image was copied
    uint64_t virtualBase; // its lowest virtual address
    uint64_t size;        // page aligned
    uint64_t entry;
};

Kernel loadKernel(FileData const& file);

// MARK: Graphics

void setupGraphics(Config const& config, Shool::Framebuffer& out);

// The trident shown while the kernel is being awakened.
void drawTrident(Shool::Framebuffer const& fb);

// MARK: Hand-over

// Builds the kernel's first page tables (identity + direct map of every
// memory region, plus the kernel image). Returns the PML4's address.
uint64_t buildPageTables(Kernel const& kernel, Shool::Framebuffer const& fb);

// Exits boot services and fills `info` with the final memory map.
void exitBootServices(Shool::BootInfo& info);

[[noreturn]] void enterKernel(uint64_t pml4, uint64_t stackTop, uint64_t entry, Shool::BootInfo* info);

} // namespace Loader
