#pragma once

// Kapalaframe: typed messages over RudraCore channels.
//
//   struct Ping { unsigned long n; };
//   channel.send(Ping{42});
//   Ping ping;
//   if (channel.recv(ping) == Sys::OK) ...

#include <kapala/sys.h>

namespace Kapala {

class Channel {
public:
    Channel() = default;
    explicit Channel(Sys::Handle handle) : _handle(handle) {}

    Channel(Channel&& other) : _handle(other._handle) { other._handle = Sys::NO_HANDLE; }
    Channel(Channel const&) = delete;
    Channel& operator=(Channel const&) = delete;

    ~Channel() {
        if (_handle != Sys::NO_HANDLE)
            Sys::close(_handle);
    }

    // Two connected channels.
    static long createPair(Channel& a, Channel& b) {
        Sys::Handle ha, hb;
        long result = Sys::channelCreate(ha, hb);
        if (result == Sys::OK) {
            a = Channel(ha);
            b = Channel(hb);
        }
        return result;
    }

    Channel& operator=(Channel&& other) {
        if (this != &other) {
            if (_handle != Sys::NO_HANDLE)
                Sys::close(_handle);
            _handle = other._handle;
            other._handle = Sys::NO_HANDLE;
        }
        return *this;
    }

    Sys::Handle handle() const { return _handle; }

    // Gives up ownership, e.g. to pass the handle to another program.
    Sys::Handle release() {
        Sys::Handle h = _handle;
        _handle = Sys::NO_HANDLE;
        return h;
    }

    template <typename T>
    long send(T const& message, Sys::Handle attach = Sys::NO_HANDLE) {
        static_assert(sizeof(T) <= Sys::MAX_MESSAGE_SIZE, "message too big for a channel");
        return Sys::send(_handle, &message, sizeof(T), attach);
    }

    // Waits for a message of type T. Returns OK, or a negative Error
    // (INVALID_ARGUMENT if the message wasn't the size of a T).
    template <typename T>
    long recv(T& message, Sys::Handle* attached = nullptr, unsigned long flags = 0) {
        long size = Sys::recv(_handle, &message, sizeof(T), attached, flags);
        if (size < 0)
            return size;
        return static_cast<unsigned long>(size) == sizeof(T) ? Sys::OK : Sys::INVALID_ARGUMENT;
    }

private:
    Sys::Handle _handle = Sys::NO_HANDLE;
};

} // namespace Kapala
