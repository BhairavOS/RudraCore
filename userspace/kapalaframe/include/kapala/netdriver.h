#pragma once

// Kapalaframe: the main loop every network card driver shares. A driver
// implements the card and calls runNetDriver:
//
//   struct Card {
//       bool start(Sys::Handle device);
//       unsigned char const* mac() const;
//       bool linkUp();
//       bool send(unsigned char const* frame, unsigned length); // false: card busy
//       template <typename F> void receive(F deliver);         // deliver(frame, length)
//       void acknowledgeInterrupt();                            // lowers the interrupt line
//   };
//
// Handles every driver gets from init: 1 the PCI device, 2 a channel to the
// net server, 3 the interrupt (optional; without it the card is polled).

#include <kapala/channel.h>
#include <kapala/protocol/netdev.h>
#include <kapala/sys.h>
#include <kapala/text.h>

namespace Kapala {

template <typename Card>
int runNetDriver(Card& card, char const* name) {
    namespace Dev = Protocol::NetDev;
    constexpr Sys::Handle DEVICE = 1, NET = 2, IRQ = 3;

    Channel net(NET);
    if (not card.start(DEVICE)) {
        Text<> t;
        t.add(name).add(": the card didn't start");
        Sys::log(t.cstr());
        return 1;
    }

    // The net server sends the shared rings first.
    Dev::Shared* shared = nullptr;
    while (not shared) {
        Dev::Message m{};
        Sys::Handle memory = Sys::NO_HANDLE;
        if (net.recv(m, &memory) != Sys::OK)
            return 1;
        if (m.type == Dev::Type::Attach and memory)
            shared = static_cast<Dev::Shared*>(Sys::memoryMap(memory));
        if (memory)
            Sys::close(memory); // the mapping keeps it
    }

    auto sendInfo = [&](bool up) {
        Dev::Message info{};
        info.type = Dev::Type::Info;
        info.linkUp = up;
        for (int i = 0; i < 6; i++)
            info.mac[i] = card.mac()[i];
        for (int i = 0; name[i] and i < 23; i++)
            info.name[i] = name[i];
        net.send(info);
    };
    bool up = card.linkUp();
    sendInfo(up);

    Channel irqEvents, irqSource;
    bool haveIrq = Channel::createPair(irqEvents, irqSource) == Sys::OK and
                   Sys::irqBind(IRQ, irqSource.handle(), 1) == Sys::OK;
    {
        Text<> t;
        static constexpr char HEX[] = "0123456789abcdef";
        t.add(name).add(" ready, mac ");
        for (int i = 0; i < 6; i++) {
            char b[3] = {HEX[card.mac()[i] >> 4], HEX[card.mac()[i] & 15], 0};
            t.add(b).add(i < 5 ? ":" : "");
        }
        t.add(up ? ", link up" : ", link down").add(haveIrq ? "" : ", polling (no interrupt)");
        Sys::log(t.cstr());
    }

    for (;;) {
        Sys::Handle handles[2];
        unsigned long count = 0;
        handles[count++] = net.handle();
        if (haveIrq)
            handles[count++] = irqEvents.handle();
        bool sending = shared->send.head != shared->send.tail;
        long ready = Sys::wait(handles, count, not haveIrq or sending ? 2 : 50);

        if (ready == 0) {
            Dev::Message m{};
            long r = net.recv(m, nullptr, Sys::RECV_NONBLOCK);
            if (r == Sys::PEER_CLOSED)
                return 0;
        } else if (ready == 1) {
            Sys::IrqMessage message;
            irqEvents.recv(message);
            card.acknowledgeInterrupt();
            Sys::irqAck(IRQ);
        }

        bool received = false;
        card.receive([&](unsigned char const* frame, unsigned length) {
            if (Dev::push(shared->receive, frame, length)) // (dropped if net is behind)
                received = true;
        });
        if (received) {
            Dev::Message kick{};
            kick.type = Dev::Type::Kick;
            net.send(kick); // QUEUE_FULL is fine: a kick is already waiting
        }

        Dev::drain(shared->send, [&](unsigned char const* frame, unsigned length) {
            return length == 0 or card.send(frame, length);
        });

        bool nowUp = card.linkUp();
        if (nowUp != up) {
            up = nowUp;
            sendInfo(up);
        }
    }
}

} // namespace Kapala
