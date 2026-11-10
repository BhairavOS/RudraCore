#include "serial.h"

#include "arch/x86_64/io.h"

namespace Rudracore::Serial {

static constexpr u16 COM1 = 0x3F8;

void init() {
    using Arch::outb;
    outb(COM1 + 1, 0x00); // Disable interrupts
    outb(COM1 + 3, 0x80); // Enable DLAB to set the baud divisor
    outb(COM1 + 0, 0x03); // Divisor low byte: 38400 baud
    outb(COM1 + 1, 0x00); // Divisor high byte
    outb(COM1 + 3, 0x03); // 8 bits, no parity, one stop bit
    outb(COM1 + 2, 0xC7); // Enable and clear FIFOs, 14-byte threshold
    outb(COM1 + 4, 0x0B); // RTS/DSR set
}

static void writeRaw(char c) {
    while ((Arch::inb(COM1 + 5) & 0x20) == 0)
        ; // Wait for the transmit buffer to be empty
    Arch::outb(COM1, static_cast<u8>(c));
}

void write(char c) {
    if (c == '\n')
        writeRaw('\r');
    writeRaw(c);
}

} // namespace Rudracore::Serial
