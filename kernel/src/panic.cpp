#include "panic.h"

#include "arch/x86_64/cpu.h"
#include "arch/x86_64/io.h"
#include "display.h"
#include "log.h"

extern "C" u8 __kernel_start[];
extern "C" u8 __kernel_end[];

namespace Rudracore {

namespace {

bool panicking = false;

// Frames live on kernel stacks, which are inside the kernel image (.bss).
bool isValidFrame(u64 rbp) {
    auto start = reinterpret_cast<u64>(__kernel_start);
    auto end = reinterpret_cast<u64>(__kernel_end);
    return rbp >= start and rbp + 16 <= end and (rbp % 8) == 0;
}

} // namespace

namespace Panic {

void begin(char const* title) {
    Arch::disableInterrupts();
    if (panicking) {
        log("");
        log("!!! panic while panicking: ", title);
        Arch::halt();
    }
    panicking = true;

    Display::showPanicScreen(title);
    log("");
    log("!!! RudraCore panic: ", title);
}

void backtrace(u64 rbp) {
    log("");
    log("  backtrace:");
    for (int depth = 0; depth < 16 and isValidFrame(rbp); depth++) {
        auto* frame = reinterpret_cast<u64*>(rbp);
        u64 returnAddress = frame[1];
        if (returnAddress == 0)
            break;
        log("    #", depth, "  ", Hex{returnAddress});
        rbp = frame[0];
    }
}

void end() {
    log("");
    log("  system halted.");
    Arch::halt();
}

} // namespace Panic

void panic(char const* message) {
    Panic::begin("kernel panic");
    log(message);
    Panic::backtrace(Arch::currentFramePointer());
    Panic::end();
}

} // namespace Rudracore
