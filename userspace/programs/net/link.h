#pragma once

// The network card, as the stack sees it: a driver program on the other
// end of a channel, with frames passing through shared rings
// (kapala/protocol/netdev.h).

#include <kapala/channel.h>
#include <kapala/protocol/netdev.h>
#include <kapala/sys.h>

namespace Net {

using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned;
using u64 = unsigned long;

class Link {
public:
    static constexpr u32 MAX_FRAME = Kapala::Protocol::NetDev::MAX_FRAME;

    // Shares the rings with the driver and waits for its Info.
    bool attach(Kapala::Sys::Handle driver);

    Kapala::Sys::Handle channel() const { return _driver.handle(); }
    u8 const* mac() const { return _mac; }
    bool up() const { return _up; }
    char const* name() const { return _name; }

    // Queues a frame for the driver (sent at the next flush). False if the
    // ring is full: the frame is dropped, like a busy card would.
    bool send(u8 const* frame, u32 length);

    // Wakes the driver if frames were queued.
    void flush();

    // Handles a message from the driver. False once the driver is gone.
    bool service();

    // Calls deliver(frame, length) for every received frame.
    template <typename F>
    void receive(F deliver) {
        if (_shared)
            Kapala::Protocol::NetDev::drain(_shared->receive, [&](u8 const* frame, u32 length) {
                if (length)
                    deliver(frame, length);
                return true;
            });
    }

private:
    Kapala::Channel _driver;
    Kapala::Protocol::NetDev::Shared* _shared = nullptr;
    u8 _mac[6] = {};
    bool _up = false, _needKick = false;
    char _name[24] = {};
};

} // namespace Net
