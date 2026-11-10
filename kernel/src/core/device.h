#pragma once

#include "core/object.h"

namespace Rudracore::Core {

struct Task;

// A hardware interrupt line delivered to a userspace driver.
//
// When the IRQ fires, the kernel masks the line and wakes the driver. The
// line stays masked while the driver services the device, and is unmasked
// when the driver calls irq_wait again. So a driver is never interrupted by
// its own device mid-work and can't miss an edge.
struct Irq : public Object {
    u8 line;
    bool pending = false;
    Task* waiter = nullptr;

    explicit Irq(u8 line);
    ~Irq() override;

    // Called from the interrupt handler. Returns false if no driver owns
    // the line.
    static bool deliver(u8 line);
};

// A range of x86 I/O ports a driver may use.
struct IoPorts : public Object {
    u16 base;
    u16 count;

    IoPorts(u16 base, u16 count) : Object(ObjectType::IoPorts), base(base), count(count) {}

    bool contains(u16 port) const { return port >= base and port - base < count; }
};

} // namespace Rudracore::Core
