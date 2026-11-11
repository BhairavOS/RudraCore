#include "arch/x86_64/pci.h"

namespace Rudracore::Arch::Pci {

namespace {

constexpr u16 CONFIG_ADDRESS = 0xCF8;
constexpr u16 CONFIG_DATA = 0xCFC;

inline void outl(u16 port, u32 value) {
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

inline u32 inl(u16 port) {
    u32 value;
    asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void select(Address a, u8 offset) {
    outl(CONFIG_ADDRESS, 0x80000000u | (u32{a.bus} << 16) | (u32{a.device} << 11) |
                             (u32{a.function} << 8) | (offset & 0xFC));
}

} // namespace

u32 read32(Address a, u8 offset) {
    select(a, offset);
    return inl(CONFIG_DATA);
}

void write32(Address a, u8 offset, u32 value) {
    select(a, offset);
    outl(CONFIG_DATA, value);
}

u16 read16(Address a, u8 offset) {
    return static_cast<u16>(read32(a, offset) >> ((offset & 2) * 8));
}

void write16(Address a, u8 offset, u16 value) {
    u32 shift = (offset & 2) * 8;
    u32 old = read32(a, offset);
    write32(a, offset, (old & ~(0xFFFFu << shift)) | (u32{value} << shift));
}

Bar readBar(Address a, u8 index) {
    Bar bar;
    if (index > 5)
        return bar;
    u8 offset = static_cast<u8>(0x10 + index * 4);

    // Sizing writes all ones to the BAR, so keep the device from decoding
    // addresses meanwhile.
    u16 command = read16(a, 0x04);
    write16(a, 0x04, command & ~0x3);

    u32 low = read32(a, offset);
    write32(a, offset, ~0u);
    u32 lowMask = read32(a, offset);
    write32(a, offset, low);

    if (low & 1) {
        bar.io = true;
        bar.address = low & ~0x3u;
        bar.size = (~(lowMask & ~0x3u) + 1) & 0xFFFF;
    } else {
        bool wide = ((low >> 1) & 3) == 2;
        u64 address = low & ~0xFu;
        u64 mask = 0xFFFFFFFF00000000ull | (lowMask & ~0xFu);
        if (wide and index < 5) {
            u8 highOffset = static_cast<u8>(offset + 4);
            u32 high = read32(a, highOffset);
            write32(a, highOffset, ~0u);
            u32 highMask = read32(a, highOffset);
            write32(a, highOffset, high);
            address |= u64{high} << 32;
            mask = (u64{highMask} << 32) | (lowMask & ~0xFu);
        }
        bar.address = address;
        bar.size = address ? ~mask + 1 : 0;
    }

    write16(a, 0x04, command);
    if (not bar.address)
        bar.size = 0;
    return bar;
}

} // namespace Rudracore::Arch::Pci
