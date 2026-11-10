#include "core/device.h"

#include "arch/x86_64/pic.h"
#include "core/sched.h"
#include "panic.h"

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
    if (waiter)
        Sched::wake(waiter); // can't happen today: a blocked task holds a handle
}

bool Irq::deliver(u8 line) {
    Irq* irq = line < 16 ? owners[line] : nullptr;
    if (not irq)
        return false;

    Arch::Pic::mask(line);
    irq->pending = true;
    if (Task* t = irq->waiter) {
        irq->waiter = nullptr;
        Sched::wake(t);
    }
    return true;
}

} // namespace Rudracore::Core
