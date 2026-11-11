// e1000: the driver for Intel e1000 network cards (82540EM and relatives:
// QEMU's default card, VirtualBox's and VMware's, and many older PCs).
//
//   handle 1: the card (PCI device)
//   handle 2: a channel to the net server (protocol/netdev.h)
//   handle 3: the card's interrupt (optional)

#include "e1000.h"

#include <kapala/netdriver.h>

namespace {

// The driver loop's view of the card.
struct Card {
    Driver::E1000 e1000;

    bool start(Kapala::Sys::Handle device) { return e1000.start(device); }
    unsigned char const* mac() const { return e1000.mac(); }
    bool linkUp() { return e1000.linkUp(); }
    bool send(unsigned char const* frame, unsigned length) { return e1000.send(frame, length); }
    void acknowledgeInterrupt() { e1000.acknowledgeInterrupt(); }

    template <typename F>
    void receive(F deliver) {
        e1000.receive([&](unsigned char const* frame, unsigned length) { deliver(frame, length); });
    }
};

Card card;

} // namespace

int main() { return Kapala::runNetDriver(card, "Intel e1000"); }
