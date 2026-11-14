#include "arch/x86_64/pic.h"

#include "arch/x86_64/apic.h"
#include "arch/x86_64/io.h"

namespace Rudracore::Arch::Pic {

namespace {

constexpr u16 MASTER_CMD = 0x20, MASTER_DATA = 0x21;
constexpr u16 SLAVE_CMD = 0xA0, SLAVE_DATA = 0xA1;

constexpr u8 ICW1_INIT = 0x11; // init, ICW4 follows
constexpr u8 ICW4_8086 = 0x01;
constexpr u8 CMD_EOI = 0x20;
constexpr u8 CMD_READ_ISR = 0x0B;

void wait() { outb(0x80, 0); } // a short delay for old hardware

void write(u16 port, u8 value) {
    outb(port, value);
    wait();
}

} // namespace

void init() {
    write(MASTER_CMD, ICW1_INIT);
    write(SLAVE_CMD, ICW1_INIT);
    write(MASTER_DATA, IRQ_BASE);     // master vectors 32-39
    write(SLAVE_DATA, IRQ_BASE + 8);  // slave vectors 40-47
    write(MASTER_DATA, 4);            // slave on IRQ 2
    write(SLAVE_DATA, 2);             // slave cascade identity
    write(MASTER_DATA, ICW4_8086);
    write(SLAVE_DATA, ICW4_8086);

    // Mask everything except the cascade line, so the slave can be unmasked later.
    write(MASTER_DATA, 0xFF & ~(1 << 2));
    write(SLAVE_DATA, 0xFF);
}

void setLevelTriggered(u8 irq) {
    if (Apic::active())
        return; // (the I/O APIC's entries say it)
    if (irq < 3 or irq >= 16 or irq == 8 or irq == 13)
        return; // timer, keyboard, cascade, clock and FPU lines must stay edge-triggered
    u16 port = irq < 8 ? 0x4D0 : 0x4D1;
    outb(port, static_cast<u8>(inb(port) | (1 << (irq & 7))));
}

void unmask(u8 irq) {
    if (Apic::active())
        return Apic::unmask(irq);
    u16 port = irq < 8 ? MASTER_DATA : SLAVE_DATA;
    outb(port, inb(port) & ~(1 << (irq % 8)));
}

void mask(u8 irq) {
    if (Apic::active())
        return Apic::mask(irq);
    u16 port = irq < 8 ? MASTER_DATA : SLAVE_DATA;
    outb(port, inb(port) | (1 << (irq % 8)));
}

void endOfInterrupt(u8 irq) {
    if (Apic::active())
        return Apic::endOfInterrupt();
    if (irq >= 8)
        outb(SLAVE_CMD, CMD_EOI);
    outb(MASTER_CMD, CMD_EOI);
}

bool isSpurious(u8 irq) {
    if (Apic::active() or (irq != 7 and irq != 15))
        return false;

    u16 cmd = irq == 7 ? MASTER_CMD : SLAVE_CMD;
    outb(cmd, CMD_READ_ISR);
    bool inService = inb(cmd) & (1 << 7);
    if (inService)
        return false;

    // A spurious IRQ from the slave still needs an EOI on the master.
    if (irq == 15)
        outb(MASTER_CMD, CMD_EOI);
    return true;
}

} // namespace Rudracore::Arch::Pic
