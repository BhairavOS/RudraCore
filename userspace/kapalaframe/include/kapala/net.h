#pragma once

// Kapalaframe: the network, through the net server.
//
//   Net net;
//   if (net.connect(3)) {                       // apps get the connection as handle 3
//       unsigned address;
//       net.resolve("example.com", address);
//       long socket = net.tcpConnect(address, 80);
//       net.send(socket, request, length);
//       long n = net.receive(socket, buffer, sizeof(buffer));  // 0: closed
//       net.close(socket);
//   }
//
// Every call returns a count or id (>= 0), or a negative Sys::Error.

#include <kapala/channel.h>
#include <kapala/protocol/net.h>
#include <kapala/text.h>

namespace Kapala {

struct NetStatus {
    bool up; // has an address
    unsigned address, netmask, gateway, dns;
    unsigned char mac[6];
};

class Net {
public:
    bool connect(Sys::Handle channel);
    bool connected() const { return _buffer != nullptr; }

    long status(NetStatus& out);

    // Waits up to `timeoutMs` for the network to come up (DHCP).
    bool waitUntilUp(unsigned timeoutMs);

    // A host name ("example.com") or dotted address ("10.0.2.2").
    long resolve(char const* host, unsigned& address, unsigned timeoutMs = 5000);

    // Round-trip time in milliseconds.
    long ping(unsigned address, unsigned timeoutMs = 2000);

    // Returns a socket.
    long tcpConnect(unsigned address, unsigned short port, unsigned timeoutMs = 10000);

    // Sends everything (waiting while the send buffer is full).
    long send(long socket, void const* data, unsigned long length);

    // Up to `capacity` bytes; 0 once the other side has closed.
    long receive(long socket, void* data, unsigned long capacity, unsigned timeoutMs = 30000);

    long close(long socket);

    // "10.0.2.15"
    template <unsigned long N>
    static void format(unsigned address, Text<N>& out) {
        for (int i = 3; i >= 0; i--) {
            out.add(static_cast<unsigned long>((address >> (i * 8)) & 0xFF));
            if (i)
                out.add(".");
        }
    }

    // Parses "10.0.2.15"; false if it isn't a dotted address.
    static bool parse(char const* text, unsigned& address);

private:
    long call(Protocol::Net::Request& request, Protocol::Net::Reply& reply);

    Channel _channel;
    unsigned char* _buffer = nullptr;
};

} // namespace Kapala
