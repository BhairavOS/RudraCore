#pragma once

// Kapalaframe: system call wrappers for Bhairav OS programs.
// See abi/rudracore/syscalls.h for what each call does.

#include <rudracore/syscalls.h>

// Every program's entry point, called by the Kapalaframe runtime. In a
// freestanding build main isn't special, so give it C linkage here.
extern "C" int main();

namespace Kapala::Sys {

using namespace Rudracore::Abi;

inline long syscall(unsigned long number, unsigned long a = 0, unsigned long b = 0,
                    unsigned long c = 0, unsigned long d = 0, unsigned long e = 0) {
    long result;
    register unsigned long r10 asm("r10") = d;
    register unsigned long r8 asm("r8") = e;
    asm volatile("int $0x80"
                 : "=a"(result)
                 : "a"(number), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                 : "memory");
    return result;
}

template <typename T>
inline unsigned long arg(T* pointer) { return reinterpret_cast<unsigned long>(pointer); }

inline unsigned long length(char const* text) {
    unsigned long n = 0;
    while (text[n])
        n++;
    return n;
}

// MARK: Tasks

inline long log(char const* text, unsigned long len) { return syscall(SYS_LOG, arg(text), len); }
inline long log(char const* text) { return log(text, length(text)); }

[[noreturn]] inline void exit(long code) {
    syscall(SYS_EXIT, static_cast<unsigned long>(code));
    __builtin_unreachable();
}

inline void yield() { syscall(SYS_YIELD); }
inline void sleep(unsigned long milliseconds) { syscall(SYS_SLEEP, milliseconds); }
inline unsigned long taskId() { return static_cast<unsigned long>(syscall(SYS_TASK_ID)); }
inline unsigned long timeMs() { return static_cast<unsigned long>(syscall(SYS_TIME)); }

// Starts a program from the boot image; `handles` move to it (as its
// handles 1, 2, 3...). Returns its task id, or a negative Error.
inline long spawn(char const* name, Handle const* handles = nullptr, unsigned long count = 0) {
    return syscall(SYS_SPAWN, arg(name), length(name), arg(handles), count);
}

inline long systemInfo(SystemInfo& out) { return syscall(SYS_SYSTEM_INFO, arg(&out)); }

// MARK: Handles and channels

inline long close(Handle handle) { return syscall(SYS_HANDLE_CLOSE, handle); }

inline long channelCreate(Handle& a, Handle& b) {
    Handle pair[2] = {};
    long result = syscall(SYS_CHANNEL_CREATE, arg(pair));
    a = pair[0];
    b = pair[1];
    return result;
}

inline long send(Handle endpoint, void const* data, unsigned long size, Handle attach = NO_HANDLE) {
    return syscall(SYS_CHANNEL_SEND, endpoint, arg(data), size, attach);
}

// Returns the message size; `attached` gets a handle that came with it, or NO_HANDLE.
inline long recv(Handle endpoint, void* buffer, unsigned long capacity,
                 Handle* attached = nullptr, unsigned long flags = 0) {
    return syscall(SYS_CHANNEL_RECV, endpoint, arg(buffer), capacity, arg(attached), flags);
}

// MARK: Memory

inline long memoryCreate(unsigned long size) { return syscall(SYS_MEMORY_CREATE, size); }

// Returns the address, or nullptr on failure.
inline void* memoryMap(Handle memory) {
    long result = syscall(SYS_MEMORY_MAP, memory);
    return result < 0 ? nullptr : reinterpret_cast<void*>(result);
}

// Fresh zeroed private memory, or nullptr. (Most code uses `new` instead.)
inline void* memoryAlloc(unsigned long size) {
    long result = syscall(SYS_MEMORY_ALLOC, size);
    return result < 0 ? nullptr : reinterpret_cast<void*>(result);
}

// MARK: Devices

inline long irqWait(Handle irq) { return syscall(SYS_IRQ_WAIT, irq); }

// Each interrupt now arrives as an IrqMessage{cookie} on the peer of `endpoint`.
inline long irqBind(Handle irq, Handle endpoint, unsigned long cookie) {
    return syscall(SYS_IRQ_BIND, irq, endpoint, cookie);
}

inline long irqAck(Handle irq) { return syscall(SYS_IRQ_ACK, irq); }

inline long portIn(Handle ports, unsigned short port) { return syscall(SYS_PORT_IN, ports, port); }

inline long portOut(Handle ports, unsigned short port, unsigned char value) {
    return syscall(SYS_PORT_OUT, ports, port, value);
}

} // namespace Kapala::Sys
