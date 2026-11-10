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
    message.attached = nullptr;
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

} // namespace Rudracore::Core
