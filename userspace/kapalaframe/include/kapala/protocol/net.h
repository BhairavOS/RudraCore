#pragma once

// Kapalaframe: the network protocol, between programs and the net server.
// Most programs use Kapala::Net (kapala/net.h) instead.
//
// Connecting works like the file system: whoever holds the net server's
// connect channel sends it a Connect with one end of a new channel
// attached. KamalShell connects every app it starts (the app's handle 3).
//
//   client                                          net
//   Attach + shared memory (BUFFER_SIZE bytes)   ->
//                                                <- Reply
//   Status                                       <- Reply{address, gateway, dns, mac, up}
//   Resolve{name}                                <- Reply{address}        (DNS)
//   Ping{address}                                <- Reply{value: ms}      (ICMP echo)
//   TcpConnect{address, port}                    <- Reply{socket}
//   (fills the buffer) Send{socket, length}      <- Reply{length taken}
//   Receive{socket, length: at most}             <- Reply{length}: data in the buffer,
//                                                   0 once the other side has closed
//   Close{socket}                                <- Reply
//
// Requests that wait (Resolve, Ping, TcpConnect, Receive, a Send into a full
// buffer) are answered when done, or after timeoutMs (TIMED_OUT).
// Addresses are IPv4 in host order: 10.0.2.15 is 0x0A00020F.

namespace Kapala::Protocol::Net {

constexpr unsigned long BUFFER_SIZE = 64 * 1024;
constexpr unsigned long MAX_NAME = 96;

struct Connect {
    unsigned long magic; // CONNECT_MAGIC
};
constexpr unsigned long CONNECT_MAGIC = 0x4E455457; // "NETW"

enum class Op : unsigned char {
    Attach = 1,
    Status = 2,
    Resolve = 3,
    Ping = 4,
    TcpConnect = 5,
    Send = 6,
    Receive = 7,
    Close = 8,
};

struct Request {
    Op op;
    unsigned short port;
    unsigned socket;
    unsigned address;
    unsigned length;
    unsigned timeoutMs; // 0: no limit
    char name[MAX_NAME];
};

struct Reply {
    long status; // Sys::OK or a negative Sys::Error
    unsigned socket;
    unsigned address;
    unsigned netmask;
    unsigned gateway;
    unsigned dns;
    unsigned length;
    unsigned long value;
    unsigned char mac[6];
    bool up;
};

static_assert(sizeof(Request) <= 128 and sizeof(Reply) <= 128);

} // namespace Kapala::Protocol::Net
