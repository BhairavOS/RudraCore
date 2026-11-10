#pragma once

// KapalaFrame: system call wrappers for BhairavOS programs.
// See abi/rudracore/syscalls.h for the interface definition.

#include <rudracore/syscalls.h>

// Every program's entry point, called by the KapalaFrame runtime. In a
// freestanding build main isn't special, so give it C linkage here.
extern "C" int main();

namespace Kapala::Sys {

using namespace Rudracore::Abi;

inline long syscall(unsigned long number, unsigned long a = 0, unsigned long b = 0, unsigned long c = 0) {
    long result;
    asm volatile("int $0x80"
                 : "=a"(result)
                 : "a"(number), "D"(a), "S"(b), "d"(c)
                 : "memory");
    return result;
}

inline unsigned long length(char const* text) {
    unsigned long n = 0;
    while (text[n])
        n++;
    return n;
}

inline long log(char const* text, unsigned long len) {
    return syscall(SYS_LOG, reinterpret_cast<unsigned long>(text), len);
}

inline long log(char const* text) {
    return log(text, length(text));
}

[[noreturn]] inline void exit(long code) {
    syscall(SYS_EXIT, static_cast<unsigned long>(code));
    __builtin_unreachable();
}

inline void yield() { syscall(SYS_YIELD); }

inline void sleep(unsigned long milliseconds) { syscall(SYS_SLEEP, milliseconds); }

inline unsigned long taskId() { return static_cast<unsigned long>(syscall(SYS_TASK_ID)); }

inline unsigned long timeMs() { return static_cast<unsigned long>(syscall(SYS_TIME)); }

} // namespace Kapala::Sys
