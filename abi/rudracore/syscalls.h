#pragma once

// The RudraCore system call interface, shared by the kernel and userspace
// (KapalaFrame). Calling convention: `int $0x80` with the call number in rax
// and arguments in rdi, rsi, rdx. The result comes back in rax: >= 0 on
// success, a negative Error on failure. All other registers are preserved.

namespace Rudracore::Abi {

constexpr unsigned char SYSCALL_VECTOR = 0x80;

enum Syscall : unsigned long {
    SYS_LOG = 0,     // (char const* text, usize length): write to the kernel log
    SYS_EXIT = 1,    // (long code): end the calling task
    SYS_YIELD = 2,   // (): let other tasks run
    SYS_SLEEP = 3,   // (u64 milliseconds)
    SYS_TASK_ID = 4, // () -> id of the calling task
    SYS_TIME = 5,    // () -> milliseconds since boot
};

enum Error : long {
    OK = 0,
    UNKNOWN_SYSCALL = -1,
    BAD_ADDRESS = -2,
    INVALID_ARGUMENT = -3,
};

} // namespace Rudracore::Abi
