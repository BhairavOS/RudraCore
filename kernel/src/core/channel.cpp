#include "core/channel.h"

#include "core/sched.h"

namespace Rudracore::Core {

Endpoint::~Endpoint() {
    while (not empty()) {
        if (front().attached)
            front().attached->unref();
        pop();
    }

    if (peer) {
        peer->peer = nullptr;
        peer->wakeWaiter(); // let a blocked receiver see PEER_CLOSED
    }
}

bool Endpoint::createPair(Endpoint*& a, Endpoint*& b) {
    a = new Endpoint();
    b = new Endpoint();
    a->peer = b;
    b->peer = a;
    return true;
}

void Endpoint::push(Message const& message) {
    queue[(head + count) % QUEUE_CAPACITY] = message;
    count++;
    wakeWaiter();
}

void Endpoint::pop() {
    queue[head].attached = nullptr;
    head = (head + 1) % QUEUE_CAPACITY;
    count--;
}

void Endpoint::wakeWaiter() {
    if (Task* t = waiter) {
        waiter = nullptr;
        Sched::wake(t);
    }
}

} // namespace Rudracore::Core
