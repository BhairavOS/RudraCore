#include "arch/x86_64/fpu.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/protect.h"
#include "boot.h"
#include "config.h"
#include "core/device.h"
#include "core/programs.h"
#include "core/random.h"
#include "core/sched.h"
#include "core/shared_memory.h"
#include "core/syscall.h"
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

constexpr char const* VERSION = "0.6.0";

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

// FAULT_TEST=stack|smep|smap builds: the kernel breaks one protection on
// purpose. Each must end in a panic naming the fault (see README).
[[maybe_unused]] void overflowStack(u64 depth) {
    volatile u8 buffer[512];
    buffer[0] = static_cast<u8>(depth);
    if (depth != ~0ull) // never false; keeps the compiler from calling this an infinite loop
        overflowStack(depth + 1);
    buffer[1] = buffer[0]; // not a tail call: every level keeps its frame
}

void faultTest() {
    if constexpr (RUDRACORE_FAULT_TEST == 1) {
        log("[test] a kernel task overflows its stack on purpose: the guard page must stop it");
        Sched::spawnKernel("overflow", overflowStack, 0);
    } else if constexpr (RUDRACORE_FAULT_TEST == 2 or RUDRACORE_FAULT_TEST == 3) {
        // A user page in the kernel's own (otherwise empty) lower half.
        constexpr u64 USER_PAGE = 0x400000;
        u64 page = Pmm::allocZeroed();
        *Mem::toVirtual<u8>(page) = 0xC3; // ret
        Vmm::kernelSpace().map(USER_PAGE, page, Mem::PAGE_SIZE, Vmm::USER | Vmm::EXEC);
        if constexpr (RUDRACORE_FAULT_TEST == 2) {
            log("[test] the kernel runs code in a user page on purpose: SMEP must stop it");
            reinterpret_cast<void (*)()>(USER_PAGE)();
        } else {
            log("[test] the kernel reads a user page on purpose: SMAP must stop it");
            (void)*reinterpret_cast<u8 volatile*>(USER_PAGE);
        }
        log("[test] FAILED: the kernel got through");
    }
}

// Starts init, the root of userspace, and hands it the hardware: it decides
// which driver gets what. The kernel itself never starts other programs.
void startInit(Boot::Info const& info) {
    Core::Object* handles[7] = {};
    // init gets every right; it hands each driver only the ones it needs.
    Core::Capability caps[7];

    if (info.hasFramebuffer) {
        auto const& fb = info.framebuffer;
        u64 physical = reinterpret_cast<u64>(fb.address) - info.hhdmOffset;
        auto* screen = Core::SharedMemory::device(physical, fb.pitch * fb.height, Vmm::Cache::WriteCombining);
        screen->isScreen = true;
        handles[Abi::INIT_HANDLE_FRAMEBUFFER - 1] = screen;
    }
    handles[Abi::INIT_HANDLE_PS2_PORTS - 1] = new Core::IoPorts(0x60, 5); // 0x60-0x64
    handles[Abi::INIT_HANDLE_KEYBOARD_IRQ - 1] = new Core::Irq(1);
    handles[Abi::INIT_HANDLE_MOUSE_IRQ - 1] = new Core::Irq(12);

    // The disk: the first AHCI (SATA) controller (PCI class 01, subclass 06,
    // interface 01).
    auto* storage = Core::PciDevice::find(0x01, 0x06, 0x01);
    handles[Abi::INIT_HANDLE_STORAGE - 1] = storage;
    if (storage)
        log("[pci]  storage     AHCI controller ", Hex{storage->vendor, 4}, ":", Hex{storage->device, 4},
            " at ", storage->address.bus, ":", storage->address.device, ".", storage->address.function);
    else
        log("[pci]  storage     no AHCI controller");

    // The network card: an Intel e1000 (82540EM, QEMU's default and common
    // in virtual machines, and relatives) or a Realtek RTL8139, plus its
    // interrupt line.
    static constexpr u16 E1000_IDS[] = {0x100E, 0x100F, 0x10D3, 0x1004, 0x1026, 0x1076, 0x107C, 0x1019};
    static constexpr u16 RTL8139_IDS[] = {0x8139};
    auto* network = Core::PciDevice::findById(0x8086, E1000_IDS, sizeof(E1000_IDS) / sizeof(E1000_IDS[0]));
    if (not network)
        network = Core::PciDevice::findById(0x10EC, RTL8139_IDS, 1);
    handles[Abi::INIT_HANDLE_NETWORK - 1] = network;
    if (network) {
        u8 line = network->irqLine;
        bool usable = line > 2 and line < 16 and line != 8 and line != 12 and line != 13;
        if (usable) {
            Arch::Pic::setLevelTriggered(line);
            handles[Abi::INIT_HANDLE_NETWORK_IRQ - 1] = new Core::Irq(line);
        }
        log("[pci]  network     ", network->vendor == 0x10EC ? "Realtek " : "Intel ", Hex{network->vendor, 4}, ":", Hex{network->device, 4}, " at ",
            network->address.bus, ":", network->address.device, ".", network->address.function, ", irq ",
            usable ? static_cast<u64>(line) : 0, usable ? "" : " (unusable: the driver will poll)");
    } else {
        log("[pci]  network     no supported card");
    }

    Abi::Error code;
    char const* error = nullptr;
    for (usize i = 0; i < 7; i++)
        caps[i] = {handles[i], Abi::RIGHTS_ALL, 0};
    Core::Task* init = Programs::start("init", 4, caps, 7, code, error);
    if (not init)
        panic("can't start init: is init.elf in the boot image?");
    init->privilege = Core::Privilege::Root;
    log("[core] started init as task ", init->id, " with the framebuffer, PS/2 ports, keyboard and mouse irqs",
        storage ? ", the disk controller" : "", network ? ", the network card" : "");
}

} // namespace

// Called from _start (start.S). With ShoolLoader, rdi/rsi carry the Shool
// magic and boot info; with Limine they're meaningless (see Boot::collect).
extern "C" [[noreturn]] void kmain(u64 bootMagic, u64 bootInfo) {
    Serial::init();
    runGlobalConstructors();

    Boot::Info const& info = Boot::collect(bootMagic, bootInfo);
    if (not info.protocolSupported) {
        log("rudracore: panic: unsupported bootloader (needs the Shool protocol v1 or Limine base revision 3)");
        Arch::halt();
    }
    Mem::hhdmOffset = info.hhdmOffset;

    Display::init(info);
    log("RudraCore ", VERSION, " - Efficient microkernel for Bhairav OS");
    log("");
    logBootInfo(info);

    Arch::Gdt::init();
    Arch::Idt::init();
    Arch::Pic::init();
    Arch::Fpu::init();
    Arch::Protect::init();
    log("[cpu]  gdt + tss loaded, idt ready (256 vectors), pic remapped and masked, sse on");
    testInterrupts();

    Pmm::init(info);
    Vmm::init(info);
    log("[vmm]  running on RudraCore's own page tables");
    Arch::Protect::enableSmepSmap();
    Pmm::reclaimBootloaderMemory(info);
    logMemory("");

    testMemory();
    testHeap();

    if constexpr (RUDRACORE_PANIC_TEST) {
        log("[test] writing to an unmapped address on purpose...");
        *reinterpret_cast<u64 volatile*>(0xdead0000) = 1;
    }

    Time::init();
    Random::init(Time::unixTime() ^ (reinterpret_cast<u64>(&info) << 17));
    log("[rand] random numbers from ", Random::hasHardwareSource() ? "RDRAND and " : "",
        "cycle-counter timing");
    Sched::init();
    Programs::init(info);
    Syscall::init(info);
    Sched::spawnKernel("clock", clockTask, 0);
    startInit(info);
    faultTest();

    log("[sched] starting: 1 ms timer, 10 ms time slices");
    log("");
    Sched::idle();
}
