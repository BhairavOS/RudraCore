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
    // The firmware range (the acpi server's): every port except those the
    // kernel and drivers own (PICs and their trigger registers, PIT, PS/2,
    // CMOS clock, PCI configuration, serial).
    bool firmware = false;

    IoPorts(u16 base, u16 count, bool firmware = false)
        : Object(ObjectType::IoPorts), base(base), count(count), firmware(firmware) {}

    bool contains(u16 port) const {
        if (port < base or port - base >= count)
            return false;
        if (not firmware)
            return true;
        bool owned = (port >= 0x20 and port <= 0x21) or (port >= 0xA0 and port <= 0xA1) or
                     (port >= 0x40 and port <= 0x43) or port == 0x60 or port == 0x64 or
                     (port >= 0x70 and port <= 0x71) or (port >= 0x4D0 and port <= 0x4D1) or
                     (port >= 0xCF8 and port <= 0xCFF) or (port >= 0x3F8 and port <= 0x3FF);
        return not owned;
    }
};

// What firmware code (ACPI's AML) needs: physical memory that isn't RAM
// (tables, chipset registers) and PCI configuration space. Held by the
// acpi server only (init gets it; apps never can).
struct Firmware : public Object {
    Firmware() : Object(ObjectType::Firmware) {}
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
