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

// Shows the error, waits (30 s, or until a key) so it can be read, then
// returns to the firmware, which tries its next boot option. Only before
// ExitBootServices.
[[noreturn]] void fail(char const* what, Efi::Status status = 0);

// The character of a key waiting in the firmware's buffer, or 0 (doesn't wait).
Efi::Char16 pendingKey();

// Waits up to `seconds` for a key; true if one came.
bool waitForKey(uint64_t seconds);

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
    bool diagnostic;
};

// Reads /boot/shool.cfg (defaults when missing):
//   kernel = /boot/rudracore.elf   (exactly once)
//   module = /boot/init.elf        (up to 32, each once)
//   resolution = 1280x800          (optional)
//   diagnostic = yes               (optional: as if D were held)
// Any error (unknown key, bad value, overlong path, control character)
// fail()s with its line number.
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

// Checks the ELF file thoroughly (headers, segments in the top 2 GiB and not
// overlapping, entry point in an executable segment, at most 64 MiB) and
// that its KernelHeader takes this loader's protocol version; fail()s
// otherwise. Then copies it into fresh kernel memory.
Kernel loadKernel(FileData const& file);

// fail()s if a ".elf" module isn't a well-formed x86_64 ELF executable
// (other modules are data). The kernel checks programs fully at spawn;
// this catches a broken boot image early, with a readable message.
void checkModule(char const* path, FileData const& file);

// MARK: Graphics

void setupGraphics(Config const& config, Shool::Framebuffer& out);

// The trident shown while the kernel is being awakened.
void drawTrident(Shool::Framebuffer const& fb);

// MARK: Hand-over

// Builds the kernel's first page tables (identity + direct map of every
// memory region, plus the kernel image). Returns the PML4's address.
uint64_t buildPageTables(Kernel const& kernel, Shool::Framebuffer const& fb);

// Before handing over: fail()s unless the CPU has what the kernel needs (NX)
// and the new page tables map the kernel, its entry point, stack, boot info,
// modules, framebuffer and this hand-over code where the protocol says.
void checkHandover(uint64_t pml4, Kernel const& kernel, uint64_t stack, uint64_t stackSize,
                   Shool::BootInfo const* info);

// Exits boot services and fills `info` with the final memory map.
void exitBootServices(Shool::BootInfo& info);

[[noreturn]] void enterKernel(uint64_t pml4, uint64_t stackTop, uint64_t entry, Shool::BootInfo* info);

} // namespace Loader
