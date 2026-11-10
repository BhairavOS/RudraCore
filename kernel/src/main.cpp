#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/pic.h"
#include "boot.h"
#include "config.h"
#include "core/elf.h"
#include "core/sched.h"
#include "core/time.h"
#include "display.h"
#include "log.h"
#include "mem/heap.h"
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

constexpr char const* VERSION = "0.3.0";

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
    log("[boot] modules     ", info.moduleCount);
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

// Allocates objects of many sizes (small and multi-page), checks they don't
// overlap, frees them, and checks the heap is back where it started.
void testHeap() {
    u64 before = Heap::stats().bytesInUse;

    constexpr usize COUNT = 12;
    u8* blocks[COUNT];
    for (usize i = 0; i < COUNT; i++) {
        usize size = 8u << i; // 8 bytes .. 16 KiB
        blocks[i] = static_cast<u8*>(Heap::alloc(size));
        if (not blocks[i])
            panic("selftest: heap allocation failed");
        for (usize j = 0; j < size; j++)
            blocks[i][j] = static_cast<u8>(i);
    }
    for (usize i = 0; i < COUNT; i++) {
        usize size = 8u << i;
        for (usize j = 0; j < size; j++) {
            if (blocks[i][j] != static_cast<u8>(i))
                panic("selftest: heap blocks overlap");
        }
        Heap::free(blocks[i]);
    }

    if (Heap::stats().bytesInUse != before)
        panic("selftest: heap accounting is off");
    log("[heap] small and multi-page allocations ok");
}

// Kernel task: keeps the status bar up to date. Shows the scheduler and the
// timer working: it runs every 250 ms no matter what else is going on.
void clockTask(u64) {
    for (;;) {
        Display::drawStatus(Time::now(), Sched::taskCount(),
                            Pmm::stats().freePages * Mem::PAGE_SIZE,
                            Heap::stats().bytesInUse);
        Sched::sleep(250);
    }
}

// Kernel task: spins without ever yielding for 2 seconds. Other tasks (the
// clock, user programs) must keep running meanwhile: that's preemption.
void busyTask(u64) {
    u64 start = Time::now();
    u64 loops = 0;
    while (Time::now() - start < 2000)
        loops++;
    log("[busy] spun ", loops, " loops for 2 s without yielding; others kept running");
}

void startUserPrograms(Boot::Info const& info) {
    for (usize i = 0; i < info.moduleCount; i++) {
        auto const& module = info.modules[i];
        Elf::Program program;
        char const* error = nullptr;
        if (not Elf::load(module.data, module.size, program, error)) {
            log("[elf]  can't load ", module.name, ": ", error);
            continue;
        }

        auto* task = Sched::spawnUser(module.name, program.space, program.entry, program.stackTop);
        if (task)
            log("[elf]  started ", module.name, " as task ", task->id, " (entry ", Hex{program.entry}, ")");
        else
            log("[elf]  out of memory starting ", module.name);
    }
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
    log("RudraCore ", VERSION, " - the BhairavOS microkernel");
    log("");
    logBootInfo(info);

    Arch::Gdt::init();
    Arch::Idt::init();
    Arch::Pic::init();
    log("[cpu]  gdt + tss loaded, idt ready (256 vectors), pic remapped and masked");
    testInterrupts();

    Pmm::init(info);
    Vmm::init(info);
    log("[vmm]  running on RudraCore's own page tables");
    Pmm::reclaimBootloaderMemory(info);
    logMemory("");

    testMemory();
    testHeap();

    if constexpr (RUDRACORE_PANIC_TEST) {
        log("[test] writing to an unmapped address on purpose...");
        *reinterpret_cast<u64 volatile*>(0xdead0000) = 1;
    }

    Time::init();
    Sched::init();
    Sched::spawnKernel("clock", clockTask, 0);
    Sched::spawnKernel("busy", busyTask, 0);
    startUserPrograms(info);

    log("[sched] starting: 1 ms timer, 10 ms time slices");
    log("");
    Sched::idle();
}
