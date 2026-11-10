#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/pic.h"
#include "boot.h"
#include "config.h"
#include "display.h"
#include "log.h"
#include "mem/memory.h"
#include "mem/pmm.h"
#include "mem/vmm.h"
#include "panic.h"
#include "serial.h"

using namespace Rudracore;

// Global constructors, collected by the linker script.
extern "C" void (*__init_array_start[])();
extern "C" void (*__init_array_end[])();

namespace {

constexpr char const* VERSION = "0.2.0";

void runGlobalConstructors() {
    for (auto* ctor = __init_array_start; ctor < __init_array_end; ctor++)
        (*ctor)();
}

void logBootInfo(Boot::Info const& info) {
    log("[boot] loader      ", info.loaderName, " ", info.loaderVersion);
    if (info.hasFramebuffer) {
        auto const& fb = info.framebuffer;
        log("[boot] display     ", fb.width, "x", fb.height, " ", fb.bpp, "bpp");
    } else {
        log("[boot] display     none (serial only)");
    }
    log("[boot] kernel      phys ", Hex{info.kernelPhysicalBase}, " virt ", Hex{info.kernelVirtualBase});
}

void logMemory(char const* label) {
    auto s = Pmm::stats();
    log("[pmm]  ", label, Bytes{s.freePages * Mem::PAGE_SIZE}, " free of ",
        Bytes{s.managedPages * Mem::PAGE_SIZE});
}

// Checks that an interrupt can be taken and returned from: int3 is handled
// as a non-fatal breakpoint, so execution must continue right after it.
void testInterrupts() {
    asm volatile("int3");
    log("[cpu]  interrupt round-trip ok");
}

// Allocates pages, maps one at a fresh address, checks the data is visible
// through both mappings, then cleans everything up.
void testMemory() {
    u64 freeBefore = Pmm::stats().freePages;
    u64 tablesBefore = Vmm::tablePages();

    u64 page = Pmm::alloc();
    u64 block = Pmm::alloc(4);
    if (not page or not block or page == block)
        panic("selftest: page allocation failed");

    constexpr u64 TEST_ADDRESS = 0xffffc00000000000;
    auto& space = Vmm::kernelSpace();
    if (not space.map(TEST_ADDRESS, page, Mem::PAGE_SIZE, Vmm::READ | Vmm::WRITE))
        panic("selftest: mapping a page failed");

    *reinterpret_cast<u64 volatile*>(TEST_ADDRESS) = 0xB4A1A40057ull;
    u64 phys = 0;
    if (not space.translate(TEST_ADDRESS, phys) or phys != page)
        panic("selftest: translate returned the wrong address");
    if (*Mem::toVirtual<u64 volatile>(page) != 0xB4A1A40057ull)
        panic("selftest: data written through the new mapping is missing");

    space.unmap(TEST_ADDRESS, Mem::PAGE_SIZE);
    if (space.translate(TEST_ADDRESS, phys))
        panic("selftest: page still mapped after unmap");

    Pmm::free(page);
    Pmm::free(block, 4);

    // Page tables created for the test mapping are kept for reuse (unmap
    // doesn't free them), so they're the only pages allowed to stay used.
    u64 newTables = Vmm::tablePages() - tablesBefore;
    if (Pmm::stats().freePages != freeBefore - newTables)
        panic("selftest: pages leaked");

    log("[mem]  alloc / map / translate / unmap / free ok");
}

} // namespace

extern "C" [[noreturn]] void kmain() {
    Serial::init();
    runGlobalConstructors();

    Boot::Info const& info = Boot::collect();
    if (not info.protocolSupported) {
        log("rudracore: panic: the bootloader does not support Limine base revision 3");
        Arch::halt();
    }
    Mem::hhdmOffset = info.hhdmOffset;

    Display::init(info);
    log("RudraCore ", VERSION, " - the BhairavOS kernel");
    log("");
    logBootInfo(info);

    Arch::Gdt::init();
    Arch::Idt::init();
    Arch::Pic::init();
    log("[cpu]  gdt + tss loaded, idt ready (256 vectors), pic remapped and masked");
    testInterrupts();

    Pmm::init(info);
    logMemory("after init  ");

    Vmm::init(info);
    log("[vmm]  running on RudraCore's own page tables (", Vmm::tablePages(), " table pages)");

    Pmm::reclaimBootloaderMemory(info);
    logMemory("reclaimed   ");

    testMemory();

    if constexpr (RUDRACORE_PANIC_TEST) {
        log("[test] writing to an unmapped address on purpose...");
        *reinterpret_cast<u64 volatile*>(0xdead0000) = 1;
    }

    log("");
    log("[core] step 2 complete. next: timer, scheduler, syscalls.");
    Arch::halt();
}
