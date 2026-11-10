#include "arch/x86_64/idt.h"

#include "arch/x86_64/cpu.h"
#include "arch/x86_64/frame.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/pic.h"
#include "core/sched.h"
#include "core/syscall.h"
#include "log.h"
#include "panic.h"
#include "rudracore/syscalls.h"

extern "C" u8 const interruptStubs[];

namespace Rudracore::Arch::Idt {

namespace {

struct [[gnu::packed]] Entry {
    u16 offsetLow;
    u16 selector;
    u8 ist;
    u8 attributes;
    u16 offsetMid;
    u32 offsetHigh;
    u32 reserved;
};
static_assert(sizeof(Entry) == 16);

struct [[gnu::packed]] Pointer {
    u16 limit;
    u64 base;
};

constexpr u8 INTERRUPT_GATE = 0x8E;      // present, ring 0, 64-bit interrupt gate
constexpr u8 USER_INTERRUPT_GATE = 0xEE; // same, but callable from ring 3

constexpr u64 VECTOR_BREAKPOINT = 3;
constexpr u64 VECTOR_PAGE_FAULT = 14;
constexpr u64 VECTOR_IRQ_BASE = Pic::IRQ_BASE;

alignas(16) Entry entries[256];

constexpr char const* EXCEPTION_NAMES[32] = {
    "division error",
    "debug",
    "non-maskable interrupt",
    "breakpoint",
    "overflow",
    "bound range exceeded",
    "invalid opcode",
    "device not available",
    "double fault",
    "coprocessor segment overrun",
    "invalid TSS",
    "segment not present",
    "stack-segment fault",
    "general protection fault",
    "page fault",
    "reserved",
    "x87 floating-point exception",
    "alignment check",
    "machine check",
    "SIMD floating-point exception",
    "virtualization exception",
    "control protection exception",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "hypervisor injection exception",
    "VMM communication exception",
    "security exception",
    "reserved",
};

void logRegisters(Frame const& f) {
    log("  rip ", Hex{f.rip}, "  rsp ", Hex{f.rsp}, "  rbp ", Hex{f.rbp});
    log("  rax ", Hex{f.rax}, "  rbx ", Hex{f.rbx}, "  rcx ", Hex{f.rcx});
    log("  rdx ", Hex{f.rdx}, "  rsi ", Hex{f.rsi}, "  rdi ", Hex{f.rdi});
    log("  r8  ", Hex{f.r8}, "  r9  ", Hex{f.r9}, "  r10 ", Hex{f.r10});
    log("  r11 ", Hex{f.r11}, "  r12 ", Hex{f.r12}, "  r13 ", Hex{f.r13});
    log("  r14 ", Hex{f.r14}, "  r15 ", Hex{f.r15}, "  cr3 ", Hex{readCr3()});
    log("  cs  ", Hex{f.cs}, "  ss  ", Hex{f.ss}, "  rfl ", Hex{f.rflags});
}

[[noreturn]] void exceptionPanic(Frame const& f) {
    Panic::begin("CPU exception");
    log("RudraCore caught a '", exceptionName(f.vector), "' (vector ", f.vector,
        ", error ", Hex{f.error}, ")");
    log("in ", (f.cs & 3) ? "user mode" : "kernel mode");

    if (f.vector == VECTOR_PAGE_FAULT) {
        log("");
        log("  address ", Hex{readCr2()}, " (",
            (f.error & 1) ? "protection violation" : "page not mapped", ", ",
            (f.error & 16) ? "instruction fetch" : ((f.error & 2) ? "write" : "read"), ")");
    }

    log("");
    logRegisters(f);
    Panic::backtrace(f.rbp);
    Panic::end();
}

// A user program faulted: report it and end that task. The kernel and the
// other tasks keep running.
void logUserFault(Frame const& f) {
    auto* task = Sched::current();
    log("[sched] ", task->name, " (task ", task->id, ") crashed: ", exceptionName(f.vector),
        " at ", Hex{f.rip});
    if (f.vector == VECTOR_PAGE_FAULT) {
        log("[sched]   ", (f.error & 1) ? "protection violation" : "page not mapped", " on ",
            (f.error & 16) ? "instruction fetch" : ((f.error & 2) ? "write" : "read"),
            " of ", Hex{readCr2()});
    }
}

} // namespace

char const* exceptionName(u64 vector) {
    return vector < 32 ? EXCEPTION_NAMES[vector] : "interrupt";
}

void init() {
    auto stubs = reinterpret_cast<u64>(interruptStubs);
    for (usize i = 0; i < 256; i++) {
        u64 handler = stubs + i * STUB_SIZE;
        entries[i] = {
            .offsetLow = static_cast<u16>(handler),
            .selector = Gdt::KERNEL_CODE,
            .ist = i == 8 ? Gdt::IST_DOUBLE_FAULT : u8(0),
            .attributes = i == Abi::SYSCALL_VECTOR ? USER_INTERRUPT_GATE : INTERRUPT_GATE,
            .offsetMid = static_cast<u16>(handler >> 16),
            .offsetHigh = static_cast<u32>(handler >> 32),
            .reserved = 0,
        };
    }

    Pointer pointer{sizeof(entries) - 1, reinterpret_cast<u64>(entries)};
    asm volatile("lidt %0" : : "m"(pointer));
}

} // namespace Rudracore::Arch::Idt

using namespace Rudracore;
using namespace Rudracore::Arch;

// Called by interruptCommon (interrupts.S) for every interrupt, with
// interrupts disabled. Returns the frame to resume: the same one, or another
// task's when the scheduler switches.
extern "C" Frame* interruptDispatch(Frame* frame) {
    u64 vector = frame->vector;
    bool fromUser = frame->cs & 3;

    if (vector == Idt::VECTOR_BREAKPOINT and not fromUser) {
        // Not fatal: log and resume after the int3 instruction.
        log("[cpu]  breakpoint at ", Hex{frame->rip});
        return frame;
    }

    if (vector < 32) {
        if (fromUser) {
            Idt::logUserFault(*frame);
            return Sched::exitCurrent(frame, -1);
        }
        Idt::exceptionPanic(*frame);
    }

    if (vector == Idt::VECTOR_IRQ_BASE) { // timer
        Pic::endOfInterrupt(0);
        return Sched::onTimerTick(frame);
    }

    if (vector > Idt::VECTOR_IRQ_BASE and vector < Idt::VECTOR_IRQ_BASE + 16) {
        u8 irq = static_cast<u8>(vector - Idt::VECTOR_IRQ_BASE);
        if (Pic::isSpurious(irq))
            return frame;
        log("[irq]  unhandled irq ", irq);
        Pic::endOfInterrupt(irq);
        return frame;
    }

    if (vector == Abi::SYSCALL_VECTOR)
        return Syscall::dispatch(frame);

    if (vector == Idt::YIELD_VECTOR and not fromUser)
        return Sched::switchFrom(frame);

    log("[cpu]  unexpected interrupt vector ", vector);
    return frame;
}
