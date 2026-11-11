#pragma once

#include "arch/x86_64/pci.h"
#include "core/object.h"

namespace Rudracore::Core {

struct Task;
struct Endpoint;

// A hardware interrupt line delivered to a userspace driver, either by
// waking a task blocked in irq_wait, or as a message on a bound channel.
//
// When the IRQ fires, the kernel masks the line before telling the driver.
// It stays masked while the driver services the device, and is unmasked
// when the driver waits again (irq_wait) or acknowledges (irq_ack). So a
// driver is never interrupted by its own device mid-work, at most one
// notification is ever in flight, and none are lost.
struct Irq : public Object {
    u8 line;
    bool pending = false;     // fired, but not yet seen by the driver
    Task* waiter = nullptr;   // a task blocked in irq_wait
    Endpoint* bound = nullptr; // messages go out from here (holds a reference)
    u64 cookie = 0;

    explicit Irq(u8 line);
    ~Irq() override;

    // Called from the interrupt handler. Returns false if no driver owns
    // the line.
    static bool deliver(u8 line);

    // Sends the notification message if the line is bound. Returns false
    // if it couldn't be queued (it stays pending and is retried on ack).
    bool notify();

    void bind(Endpoint* endpoint, u64 cookie);
    void acknowledge();
};

// A range of x86 I/O ports a driver may use.
struct IoPorts : public Object {
    u16 base;
    u16 count;

    IoPorts(u16 base, u16 count) : Object(ObjectType::IoPorts), base(base), count(count) {}

    bool contains(u16 port) const { return port >= base and port - base < count; }
};

// A PCI device handed to a userspace driver. Through it the driver can map
// the device's memory BARs and learn the physical address of its own shared
// memory, so the device can read and write it (DMA). A DMA-capable device
// can reach any memory, so only trusted drivers get one.
struct PciDevice : public Object {
    Arch::Pci::Address address;
    u16 vendor, device;
    u8 classCode, subclass, progIf, irqLine;
    Arch::Pci::Bar bars[6];

    explicit PciDevice(Arch::Pci::Address address);

    // Turns on memory decoding and bus mastering (DMA).
    void enable();

    // The first device of this class, or nullptr.
    static PciDevice* find(u8 classCode, u8 subclass, u8 progIf);

    // The first device with this vendor and one of these device IDs.
    static PciDevice* findById(u16 vendor, u16 const* devices, usize count);
};

} // namespace Rudracore::Core
