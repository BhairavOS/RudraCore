#include "core/device.h"

#include "arch/x86_64/pic.h"
#include "core/channel.h"
#include "core/sched.h"
#include "panic.h"
#include "rudracore/syscalls.h"

extern "C" void* memcpy(void* dest, void const* src, usize n);

namespace Rudracore::Core {

namespace {

Irq* owners[16] = {};

} // namespace

Irq::Irq(u8 line) : Object(ObjectType::Irq), line(line) {
    if (line >= 16 or owners[line])
        panic("irq: line out of range or already owned");
    owners[line] = this;
}

Irq::~Irq() {
    Arch::Pic::mask(line);
    owners[line] = nullptr;
    if (bound)
        bound->unref();
    if (waiter)
        Sched::wake(waiter); // can't happen today: a blocked task holds a handle
}

bool Irq::notify() {
    Endpoint* peer = bound ? bound->peer : nullptr;
    if (not peer or peer->full())
        return false;

    Message message{};
    Abi::IrqMessage payload{cookie};
    memcpy(message.data, &payload, sizeof(payload));
    message.size = sizeof(payload);
    message.attached = {};
    peer->push(message);
    return true;
}

bool Irq::deliver(u8 line) {
    Irq* irq = line < 16 ? owners[line] : nullptr;
    if (not irq)
        return false;

    Arch::Pic::mask(line);
    if (irq->bound) {
        irq->pending = not irq->notify();
        return true;
    }

    irq->pending = true;
    if (Task* t = irq->waiter) {
        irq->waiter = nullptr;
        Sched::wake(t);
    }
    return true;
}

void Irq::bind(Endpoint* endpoint, u64 newCookie) {
    endpoint->ref();
    if (bound)
        bound->unref();
    bound = endpoint;
    cookie = newCookie;
    pending = false;
    Arch::Pic::unmask(line);
}

void Irq::acknowledge() {
    // A notification that couldn't be queued earlier goes out now, and the
    // line stays masked until that one is acknowledged too.
    if (pending and bound) {
        pending = not notify();
        if (not pending)
            return;
    }
    Arch::Pic::unmask(line);
}

PciDevice::PciDevice(Arch::Pci::Address a) : Object(ObjectType::PciDevice), address(a) {
    u32 id = Arch::Pci::read32(a, 0x00);
    u32 classes = Arch::Pci::read32(a, 0x08);
    vendor = static_cast<u16>(id);
    device = static_cast<u16>(id >> 16);
    classCode = static_cast<u8>(classes >> 24);
    subclass = static_cast<u8>(classes >> 16);
    progIf = static_cast<u8>(classes >> 8);
    irqLine = static_cast<u8>(Arch::Pci::read32(a, 0x3C));

    for (u8 i = 0; i < 6; i++) {
        bars[i] = Arch::Pci::readBar(a, i);
        bool wide = not bars[i].io and bars[i].address and
                    ((Arch::Pci::read32(a, static_cast<u8>(0x10 + i * 4)) >> 1) & 3) == 2;
        if (wide)
            i++; // the next register holds this BAR's upper half
    }
}

void PciDevice::enable() {
    u16 command = Arch::Pci::read16(address, 0x04);
    Arch::Pci::write16(address, 0x04, command | 0x2 | 0x4); // memory space, bus master
}

PciDevice* PciDevice::find(u8 wantClass, u8 wantSubclass, u8 wantProgIf) {
    Arch::Pci::Address found{};
    bool any = false;
    Arch::Pci::forEach([&](Arch::Pci::Address a) {
        u32 classes = Arch::Pci::read32(a, 0x08);
        if (not any and (classes >> 8) == ((u32{wantClass} << 16) | (u32{wantSubclass} << 8) | wantProgIf)) {
            found = a;
            any = true;
        }
    });
    return any ? new PciDevice(found) : nullptr;
}

PciDevice* PciDevice::findById(u16 vendor, u16 const* devices, usize count) {
    Arch::Pci::Address found{};
    bool any = false;
    Arch::Pci::forEach([&](Arch::Pci::Address a) {
        u32 id = Arch::Pci::read32(a, 0x00);
        if (any or (id & 0xFFFF) != vendor)
            return;
        for (usize i = 0; i < count; i++) {
            if ((id >> 16) == devices[i]) {
                found = a;
                any = true;
            }
        }
    });
    return any ? new PciDevice(found) : nullptr;
}

} // namespace Rudracore::Core
