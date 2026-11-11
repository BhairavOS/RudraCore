#pragma once

// Kapalaframe: the network device protocol, between a network card driver
// (e1000, rtl8139) and the net server.
//
// Frames travel through two rings in memory the net server shares with the
// driver: received frames (driver -> net) and frames to send (net ->
// driver). Each ring has SLOTS slots of SLOT_SIZE bytes; the producer
// fills slot head % SLOTS and then advances head, the consumer reads slot
// tail % SLOTS and advances tail. Only the producer writes head and only the
// consumer writes tail, so no locks are needed.
//
//   net                                      driver
//   Attach + the shared memory            ->
//                                         <- Info{mac, link}
//   (fills the send ring) Kick            ->  (sends those frames)
//                                         <- Kick (the receive ring has frames)
//                                         <- Info (the link went up or down)
//
// A Kick only says "look at the rings"; one may stand for many frames.

namespace Kapala::Protocol::NetDev {

constexpr unsigned SLOTS = 64;
constexpr unsigned SLOT_SIZE = 2048;
constexpr unsigned MAX_FRAME = 1514; // Ethernet, without the CRC

struct Slot {
    unsigned length;
    unsigned char data[SLOT_SIZE - 4];
};

struct Ring {
    volatile unsigned head; // written by the producer
    unsigned char pad1[60];
    volatile unsigned tail; // written by the consumer
    unsigned char pad2[60];
    Slot slots[SLOTS];
};

struct Shared {
    Ring receive; // driver -> net
    Ring send;    // net -> driver
};

constexpr unsigned long SHARED_SIZE = sizeof(Shared);

enum class Type : unsigned char {
    Attach = 1, // net -> driver, with the shared memory attached
    Info = 2,   // driver -> net
    Kick = 3,   // either way
};

struct Message {
    Type type;
    bool linkUp;
    unsigned char mac[6];
    char name[24]; // the card, e.g. "Intel e1000"
};

// Adds a frame to a ring (producer side). False if the ring is full.
inline bool push(Ring& ring, unsigned char const* frame, unsigned length) {
    unsigned head = ring.head;
    if (head - ring.tail >= SLOTS or length > MAX_FRAME)
        return false;
    Slot& slot = ring.slots[head % SLOTS];
    for (unsigned i = 0; i < length; i++)
        slot.data[i] = frame[i];
    slot.length = length;
    asm volatile("" ::: "memory"); // the frame is written before it's published
    ring.head = head + 1;
    return true;
}

// Takes every waiting frame (consumer side), calling take(data, length).
template <typename F>
void drain(Ring& ring, F take) {
    unsigned tail = ring.tail;
    while (tail != ring.head) {
        asm volatile("" ::: "memory");
        Slot& slot = ring.slots[tail % SLOTS];
        unsigned length = slot.length <= MAX_FRAME ? slot.length : 0;
        if (not take(slot.data, length))
            break; // try again later (e.g. the card is busy)
        tail++;
        asm volatile("" ::: "memory");
        ring.tail = tail;
    }
}

} // namespace Kapala::Protocol::NetDev
