#pragma once

#include "types.h"

namespace Rudracore::Arch {

inline void outb(u16 port, u8 value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

inline u8 inb(u16 port) {
    u8 value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline void outw(u16 port, u16 value) {
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

inline u16 inw(u16 port) {
    u16 value;
    asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

[[noreturn]] inline void halt() {
    for (;;)
        asm volatile("cli; hlt");
}

} // namespace Rudracore::Arch
