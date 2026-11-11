#pragma once

#include "types.h"

namespace Rudracore::Arch::Pci {

// A PCI function's address (configuration mechanism #1, I/O ports 0xCF8/0xCFC).
struct Address {
    u8 bus, device, function;
};

u32 read32(Address a, u8 offset);
void write32(Address a, u8 offset, u32 value);
u16 read16(Address a, u8 offset);
void write16(Address a, u8 offset, u16 value);

struct Bar {
    u64 address = 0; // 0: not present
    u64 size = 0;
    bool io = false; // I/O port BAR instead of memory
};

// Decodes base address register `index` (0-5), measuring its size. A
// 64-bit memory BAR also uses the next register.
Bar readBar(Address a, u8 index);

// Calls `visit` for every function on every bus.
template <typename F>
void forEach(F&& visit) {
    for (u16 bus = 0; bus < 256; bus++) {
        for (u8 device = 0; device < 32; device++) {
            Address first{static_cast<u8>(bus), device, 0};
            if ((read32(first, 0) & 0xFFFF) == 0xFFFF)
                continue;
            bool multi = (read32(first, 0x0C) >> 16) & 0x80;
            for (u8 function = 0; function < (multi ? 8 : 1); function++) {
                Address a{static_cast<u8>(bus), device, function};
                if ((read32(a, 0) & 0xFFFF) != 0xFFFF)
                    visit(a);
            }
        }
    }
}

} // namespace Rudracore::Arch::Pci
