// net: the network server. Drives the network card (e1000.cpp) and runs
// the TCP/IP stack (stack.cpp), and serves programs over channels
// (kapala/protocol/net.h).
//
// Handles from init:
//   1: the network card (PCI device)
//   2: the connect channel: each message is a Connect with a new client's
//      channel attached
//   3: the card's interrupt line (optional: without it the card is polled)
//
// The card driver lives in the same program as the stack for speed; it
// only talks to the stack through E1000::send and E1000::receive.

#include "e1000.h"
#include "stack.h"

#include <kapala/channel.h>
#include <kapala/protocol/net.h>
#include <kapala/sys.h>
#include <kapala/text.h>

extern "C" void* memcpy(void* dest, void const* src, unsigned long n);

using namespace Kapala;
namespace Proto = Kapala::Protocol::Net;
using Net::u32;
using Net::u64;
using Net::u8;

namespace {

constexpr Sys::Handle DEVICE = 1;
constexpr Sys::Handle CONNECT = 2;
constexpr Sys::Handle IRQ = 3;
constexpr int MAX_CLIENTS = 16;

Net::E1000 card;
bool cardReady = false;

struct Client {
    Channel channel;
    u8* buffer = nullptr;
    bool used = false;

    // A request still waiting for its answer.
    bool waiting = false;
    Proto::Request request{};
    u64 deadline = 0;
    long pending = -1; // DNS query or ping number

    // Sockets this client opened (closed if it goes away).
    bool owns[32] = {};
};

Client clients[MAX_CLIENTS];

void reply(Client& c, Proto::Reply const& r) {
    c.waiting = false;
    c.channel.send(r);
}

void replyStatus(Client& c, long status) {
    Proto::Reply r{};
    r.status = status;
    reply(c, r);
}

bool ownsSocket(Client& c, u32 socket) { return socket < 32 and c.owns[socket]; }

// Tries to finish the client's waiting request. Returns true if answered.
bool progress(Client& c) {
    Proto::Request const& q = c.request;
    Proto::Reply r{};
    bool expired = c.deadline and Sys::timeMs() >= c.deadline;

    switch (q.op) {
    case Proto::Op::Resolve: {
        u32 address = 0;
        long status = Net::dnsResult(c.pending, address);
        if (status == Sys::WOULD_BLOCK) {
            if (not expired)
                return false;
            Net::dnsCancel(c.pending);
            status = Sys::TIMED_OUT;
        }
        r.status = status;
        r.address = address;
        break;
    }
    case Proto::Op::Ping: {
        u64 ms = 0;
        long status = Net::pingResult(c.pending, ms);
        if (status == Sys::WOULD_BLOCK) {
            if (not expired)
                return false;
            Net::pingCancel(c.pending);
            status = Sys::TIMED_OUT;
        }
        r.status = status;
        r.value = ms;
        break;
    }
    case Proto::Op::TcpConnect: {
        long error = Sys::OK;
        auto state = Net::tcpState(c.pending, error);
        if (state == Net::TcpState::Connecting and not expired)
            return false;
        if (state == Net::TcpState::Open) {
            r.status = Sys::OK;
            r.socket = static_cast<u32>(c.pending);
            c.owns[c.pending] = true;
        } else {
            Net::tcpClose(c.pending);
            r.status = state == Net::TcpState::Connecting ? Sys::TIMED_OUT : error;
        }
        break;
    }
    case Proto::Op::Send: {
        long error = Sys::OK;
        if (Net::tcpState(q.socket, error) == Net::TcpState::Closed) {
            r.status = error != Sys::OK ? error : Sys::PEER_CLOSED;
            break;
        }
        u32 taken = Net::tcpWrite(q.socket, c.buffer, q.length);
        if (taken == 0 and q.length > 0) {
            if (not expired)
                return false;
            r.status = Sys::TIMED_OUT;
            break;
        }
        r.status = Sys::OK;
        r.length = taken;
        break;
    }
    case Proto::Op::Receive: {
        u32 n = Net::tcpRead(q.socket, c.buffer, q.length);
        if (n == 0 and not Net::tcpAtEnd(q.socket)) {
            if (not expired)
                return false;
            r.status = Sys::TIMED_OUT;
            break;
        }
        long error = Sys::OK;
        Net::tcpState(q.socket, error);
        r.status = n == 0 and error != Sys::OK ? error : Sys::OK;
        r.length = n;
        break;
    }
    default: r.status = Sys::INVALID_ARGUMENT; break;
    }
    reply(c, r);
    return true;
}

void handle(Client& c, Proto::Request const& q, Sys::Handle attached) {
    if (q.op == Proto::Op::Attach) {
        u8* buffer = attached ? static_cast<u8*>(Sys::memoryMap(attached)) : nullptr;
        if (attached)
            Sys::close(attached);
        if (buffer and c.buffer)
            Sys::memoryUnmap(c.buffer);
        if (buffer)
            c.buffer = buffer;
        replyStatus(c, buffer ? Sys::OK : Sys::INVALID_ARGUMENT);
        return;
    }
    if (attached)
        Sys::close(attached);
    if (not c.buffer or q.length > Proto::BUFFER_SIZE) {
        replyStatus(c, Sys::INVALID_ARGUMENT);
        return;
    }
    if (not cardReady and q.op != Proto::Op::Status) {
        replyStatus(c, Sys::NETWORK_DOWN);
        return;
    }

    c.request = q;
    c.request.name[Proto::MAX_NAME - 1] = '\0';
    c.deadline = q.timeoutMs ? Sys::timeMs() + q.timeoutMs : 0;

    switch (q.op) {
    case Proto::Op::Status: {
        Proto::Reply r{};
        r.status = Sys::OK;
        r.up = Net::config.up and cardReady;
        r.address = Net::config.address;
        r.netmask = Net::config.netmask;
        r.gateway = Net::config.gateway;
        r.dns = Net::config.dns;
        if (cardReady)
            memcpy(r.mac, card.mac(), 6);
        reply(c, r);
        return;
    }
    case Proto::Op::Resolve: c.pending = Net::dnsStart(c.request.name); break;
    case Proto::Op::Ping: c.pending = Net::pingStart(q.address); break;
    case Proto::Op::TcpConnect: c.pending = Net::tcpOpen(q.address, q.port); break;
    case Proto::Op::Send:
    case Proto::Op::Receive:
        if (not ownsSocket(c, q.socket)) {
            replyStatus(c, Sys::BAD_HANDLE);
            return;
        }
        c.pending = 0;
        break;
    case Proto::Op::Close:
        if (not ownsSocket(c, q.socket)) {
            replyStatus(c, Sys::BAD_HANDLE);
            return;
        }
        c.owns[q.socket] = false;
        Net::tcpClose(q.socket);
        replyStatus(c, Sys::OK);
        return;
    default: replyStatus(c, Sys::INVALID_ARGUMENT); return;
    }

    if (c.pending < 0) { // couldn't even start
        replyStatus(c, c.pending);
        return;
    }
    c.waiting = true;
    progress(c);
}

void drop(Client& c) {
    for (u32 s = 0; s < 32; s++) {
        if (c.owns[s])
            Net::tcpClose(s);
    }
    if (c.waiting) {
        if (c.request.op == Proto::Op::Resolve)
            Net::dnsCancel(c.pending);
        else if (c.request.op == Proto::Op::Ping)
            Net::pingCancel(c.pending);
        else if (c.request.op == Proto::Op::TcpConnect)
            Net::tcpClose(c.pending);
    }
    if (c.buffer)
        Sys::memoryUnmap(c.buffer);
    c = {};
}

void serve(Client& c) {
    Proto::Request q;
    Sys::Handle attached = Sys::NO_HANDLE;
    long result = c.channel.recv(q, &attached, Sys::RECV_NONBLOCK);
    if (result == Sys::WOULD_BLOCK)
        return;
    if (result < 0 and result != Sys::INVALID_ARGUMENT) {
        drop(c); // gone, or a message too big to ever take
        return;
    }
    if (result == Sys::INVALID_ARGUMENT or c.waiting) { // wrong size, or one request at a time
        if (attached)
            Sys::close(attached);
        replyStatus(c, Sys::INVALID_ARGUMENT);
        return;
    }
    handle(c, q, attached);
}

bool accept(Channel& connect) {
    Proto::Connect message{};
    Sys::Handle attached = Sys::NO_HANDLE;
    long result = connect.recv(message, &attached, Sys::RECV_NONBLOCK);
    if (result == Sys::PEER_CLOSED or result == Sys::BUFFER_TOO_SMALL)
        return false;
    if (result != Sys::OK or message.magic != Proto::CONNECT_MAGIC or not attached) {
        if (attached)
            Sys::close(attached);
        return true;
    }
    for (auto& c : clients) {
        if (not c.used) {
            c = {};
            c.channel = Channel(attached);
            c.used = true;
            return true;
        }
    }
    Sys::log("too many clients");
    Sys::close(attached);
    return true;
}

} // namespace

int main() {
    Channel connect(CONNECT);

    // Interrupts arrive as messages on `irqEvents`.
    Channel irqEvents, irqSource;
    bool haveIrq = false;
    if (Channel::createPair(irqEvents, irqSource) == Sys::OK)
        haveIrq = Sys::irqBind(IRQ, irqSource.handle(), 1) == Sys::OK;

    cardReady = card.start(DEVICE);
    if (cardReady) {
        Text<> text;
        text.add("e1000 ready, mac ");
        static constexpr char HEX[] = "0123456789abcdef";
        for (int i = 0; i < 6; i++) {
            char byte[3] = {HEX[card.mac()[i] >> 4], HEX[card.mac()[i] & 15], 0};
            text.add(byte).add(i < 5 ? ":" : "");
        }
        text.add(card.linkUp() ? ", link up" : ", link down").add(haveIrq ? "" : ", polling (no interrupt)");
        Sys::log(text.cstr());
        Net::init(card);
    } else {
        Sys::log("no network card: the network is unavailable");
    }

    bool listening = true;
    for (;;) {
        Sys::Handle handles[2 + MAX_CLIENTS];
        int owners[2 + MAX_CLIENTS];
        unsigned long count = 0;
        if (haveIrq) {
            handles[count] = irqEvents.handle();
            owners[count++] = -2;
        }
        if (listening) {
            handles[count] = connect.handle();
            owners[count++] = -1;
        }
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].used) {
                handles[count] = clients[i].channel.handle();
                owners[count++] = i;
            }
        }

        // Timers (retransmits, DHCP, DNS retries) need a regular tick; a
        // card without an interrupt needs polling.
        long ready = count ? Sys::wait(handles, count, haveIrq ? 10 : 2) : Sys::TIMED_OUT;
        if (count == 0)
            Sys::sleep(10);

        if (ready >= 0) {
            int owner = owners[ready];
            if (owner == -2) {
                Sys::IrqMessage message;
                irqEvents.recv(message);
                card.acknowledgeInterrupt(); // lowers the line
                Sys::irqAck(IRQ);
            } else if (owner == -1) {
                listening = accept(connect);
            } else {
                serve(clients[owner]);
            }
        }

        if (cardReady) {
            card.receive([](u8 const* frame, u32 length) { Net::receiveFrame(frame, length); });
            Net::tick();
        }
        for (auto& c : clients) {
            if (c.used and c.waiting)
                progress(c);
        }
    }
}
