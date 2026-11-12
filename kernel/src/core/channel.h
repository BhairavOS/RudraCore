#pragma once

#include "core/object.h"
#include "rudracore/syscalls.h"

namespace Rudracore::Core {

struct Task;

struct Message {
    u8 data[Abi::MAX_MESSAGE_SIZE];
    u32 size;
    Object* attached; // a handle in transit (holds a reference), or nullptr
};

// One end of a channel. Messages sent on an endpoint are queued on its peer.
// Destroying an endpoint (its last handle closed) disconnects the peer, which
// then gets PEER_CLOSED once its queue is empty.
struct Endpoint : public Object {
    static constexpr u32 QUEUE_CAPACITY = 32;

    Endpoint* peer = nullptr;
    Message queue[QUEUE_CAPACITY];
    u32 head = 0;
    u32 count = 0;
    Task* waiter = nullptr; // a task blocked in recv on this endpoint

    Endpoint() : Object(ObjectType::Channel) {}
    ~Endpoint() override;

    // Creates two connected endpoints, each with one reference.
    static bool createPair(Endpoint*& a, Endpoint*& b);

    bool full() const { return count == QUEUE_CAPACITY; }
    bool empty() const { return count == 0; }

    // Queues a message (taking over the `attached` reference) and wakes a
    // waiting receiver. The queue must not be full.
    void push(Message const& message);

    Message& front() { return queue[head]; }
    void pop();

    // True if a channel or IRQ is waiting in this endpoint's queue: then the
    // endpoint itself must not be sent anywhere (it could close a cycle).
    bool holdsLinks() const;

    void wakeWaiter();
};

} // namespace Rudracore::Core
