#pragma once

// The TCP/IP stack: Ethernet, ARP, IPv4, ICMP, UDP, a DHCP client, a DNS
// resolver and TCP (client connections). Single-threaded: the server's
// loop feeds it frames and calls tick() regularly for its timers.

#include "e1000.h"

namespace Net {

struct Config {
    bool up = false; // DHCP gave us an address
    u32 address = 0, netmask = 0, gateway = 0, dns = 0;
};

extern Config config;

void init(E1000& nic);
void receiveFrame(u8 const* frame, u32 length);
void tick(); // retries and timeouts; call at least every few milliseconds

// MARK: DNS

// Starts looking up a host name: returns a query number, or a negative error.
long dnsStart(char const* name);
// WOULD_BLOCK while waiting; otherwise the outcome (and the query is freed).
long dnsResult(long query, u32& address);
void dnsCancel(long query);

// MARK: Ping (ICMP echo)

long pingStart(u32 address); // a ping number, or a negative error
// WOULD_BLOCK while waiting, else OK with the round trip in `ms`.
long pingResult(long ping, u64& ms);
void pingCancel(long ping);

// MARK: TCP

long tcpOpen(u32 address, u16 port); // a socket number, or a negative error

enum class TcpState { Connecting, Open, Closed };
TcpState tcpState(long socket, long& error); // error: why it closed (OK if cleanly)

u32 tcpWrite(long socket, u8 const* data, u32 length); // bytes taken (0: buffer full)
u32 tcpRead(long socket, u8* out, u32 capacity);       // bytes read
bool tcpAtEnd(long socket); // the other side closed and everything was read
void tcpClose(long socket); // the program is done with it (sends what's left, then FIN)

} // namespace Net
