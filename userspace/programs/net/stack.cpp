#include "stack.h"

#include <kapala/sys.h>
#include <kapala/text.h>

extern "C" {
void* memcpy(void* dest, void const* src, unsigned long n);
void* memset(void* dest, int value, unsigned long n);
}

namespace Net {

namespace Sys = Kapala::Sys;
using Kapala::Text;

Config config;

namespace {

E1000* card = nullptr;

constexpr u16 ETHERTYPE_IP = 0x0800, ETHERTYPE_ARP = 0x0806;
constexpr u8 PROTO_ICMP = 1, PROTO_TCP = 6, PROTO_UDP = 17;
constexpr u32 BROADCAST = 0xFFFFFFFF;
constexpr u8 BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// MARK: Helpers

u16 get16(u8 const* p) { return static_cast<u16>(p[0] << 8 | p[1]); }
u32 get32(u8 const* p) { return u32{p[0]} << 24 | u32{p[1]} << 16 | u32{p[2]} << 8 | p[3]; }
void put16(u8* p, u16 v) {
    p[0] = static_cast<u8>(v >> 8);
    p[1] = static_cast<u8>(v);
}
void put32(u8* p, u32 v) {
    for (int i = 0; i < 4; i++)
        p[i] = static_cast<u8>(v >> (24 - 8 * i));
}

u32 sum(u8 const* p, u32 length, u32 total = 0) {
    for (u32 i = 0; i < length; i += 2)
        total += u32{p[i]} << 8 | (i + 1 < length ? p[i + 1] : 0);
    return total;
}

u16 finish(u32 total) {
    while (total >> 16)
        total = (total & 0xFFFF) + (total >> 16);
    return static_cast<u16>(~total);
}

// The pseudo header TCP and UDP checksums include.
u32 pseudoSum(u32 source, u32 destination, u8 protocol, u32 length) {
    return (source >> 16) + (source & 0xFFFF) + (destination >> 16) + (destination & 0xFFFF) + protocol + length;
}

u64 now() { return Sys::timeMs(); }

u32 randomish() {
    static u32 state = 0;
    state = state * 1664525u + 1013904223u + static_cast<u32>(now());
    return state ^ (state >> 13);
}

void log(char const* message) { Sys::log(message); }

void logAddress(Text<>& t, u32 a) {
    for (int i = 3; i >= 0; i--) {
        t.add(static_cast<unsigned long>((a >> (i * 8)) & 0xFF));
        if (i)
            t.add(".");
    }
}

// MARK: Ethernet and ARP

void sendFrame(u8 const* destination, u16 type, u8 const* payload, u32 length) {
    static u8 frame[E1000::MAX_FRAME];
    if (length > E1000::MAX_FRAME - 14)
        return;
    memcpy(frame, destination, 6);
    memcpy(frame + 6, card->mac(), 6);
    put16(frame + 12, type);
    memcpy(frame + 14, payload, length);
    u32 total = 14 + length;
    if (total < 60) {
        memset(frame + total, 0, 60 - total);
        total = 60;
    }
    card->send(frame, total);
}

struct ArpEntry {
    u32 address = 0;
    u8 mac[6] = {};
    bool valid = false;
};
ArpEntry arpCache[16];
u32 arpNextSlot = 0;

// IP packets waiting for an address to be resolved.
struct Waiting {
    bool used = false;
    u32 nextHop = 0;
    u64 since = 0, lastAsk = 0;
    u32 length = 0;
    u8 packet[1500];
};
Waiting waiting[8];

ArpEntry* arpLookup(u32 address) {
    for (auto& e : arpCache) {
        if (e.valid and e.address == address)
            return &e;
    }
    return nullptr;
}

void arpLearn(u32 address, u8 const* mac) {
    ArpEntry* e = arpLookup(address);
    if (not e) {
        e = &arpCache[arpNextSlot];
        arpNextSlot = (arpNextSlot + 1) % 16;
    }
    e->address = address;
    memcpy(e->mac, mac, 6);
    e->valid = true;

    for (auto& w : waiting) {
        if (w.used and w.nextHop == address) {
            sendFrame(mac, ETHERTYPE_IP, w.packet, w.length);
            w.used = false;
        }
    }
}

void arpSend(u16 operation, u8 const* targetMac, u32 targetAddress) {
    u8 p[28];
    put16(p, 1);              // Ethernet
    put16(p + 2, ETHERTYPE_IP);
    p[4] = 6;
    p[5] = 4;
    put16(p + 6, operation);  // 1: request, 2: reply
    memcpy(p + 8, card->mac(), 6);
    put32(p + 14, config.address);
    memcpy(p + 18, targetMac, 6);
    put32(p + 24, targetAddress);
    sendFrame(operation == 1 ? BROADCAST_MAC : targetMac, ETHERTYPE_ARP, p, 28);
}

void receiveArp(u8 const* p, u32 length) {
    if (length < 28 or get16(p) != 1 or get16(p + 2) != ETHERTYPE_IP)
        return;
    u16 operation = get16(p + 6);
    u32 sender = get32(p + 14), target = get32(p + 24);
    if (not config.up)
        return;
    if (target == config.address or arpLookup(sender))
        arpLearn(sender, p + 8);
    if (operation == 1 and target == config.address)
        arpSend(2, p + 8, sender);
}

// MARK: IPv4

void sendIp(u32 destination, u8 protocol, u8 const* payload, u32 length) {
    static u16 nextId = 1;
    if (length > 1480)
        return;
    u8 packet[1500];
    u8* h = packet;
    h[0] = 0x45; // version 4, 20-byte header
    h[1] = 0;
    put16(h + 2, static_cast<u16>(20 + length));
    put16(h + 4, nextId++);
    put16(h + 6, 0x4000); // don't fragment
    h[8] = 64;            // time to live
    h[9] = protocol;
    put16(h + 10, 0);
    put32(h + 12, config.address);
    put32(h + 16, destination);
    put16(h + 10, finish(sum(h, 20)));
    memcpy(packet + 20, payload, length);
    u32 total = 20 + length;

    if (destination == BROADCAST or not config.up) {
        sendFrame(BROADCAST_MAC, ETHERTYPE_IP, packet, total);
        return;
    }
    bool local = (destination & config.netmask) == (config.address & config.netmask);
    u32 nextHop = local ? destination : config.gateway;
    if (ArpEntry* e = arpLookup(nextHop)) {
        sendFrame(e->mac, ETHERTYPE_IP, packet, total);
        return;
    }

    // Park the packet and ask who has that address.
    Waiting* slot = nullptr;
    for (auto& w : waiting) {
        if (not w.used) {
            slot = &w;
            break;
        }
    }
    if (slot) {
        slot->used = true;
        slot->nextHop = nextHop;
        slot->since = slot->lastAsk = now();
        slot->length = total;
        memcpy(slot->packet, packet, total);
    }
    arpSend(1, BROADCAST_MAC, nextHop);
}

void receiveIcmp(u32 source, u8 const* p, u32 length);
void receiveUdp(u32 source, u8 const* p, u32 length);
void receiveTcp(u32 source, u32 destination, u8 const* p, u32 length);

void receiveIp(u8 const* p, u32 length) {
    if (length < 20 or (p[0] >> 4) != 4)
        return;
    u32 headerLength = (p[0] & 0xF) * 4u;
    u32 total = get16(p + 2);
    if (headerLength < 20 or total < headerLength or total > length)
        return;
    if (finish(sum(p, headerLength)) != 0)
        return; // damaged header
    if ((get16(p + 6) & 0x3FFF) != 0)
        return; // a fragment: not supported
    u32 source = get32(p + 12), destination = get32(p + 16);
    bool forUs = destination == BROADCAST or (config.up and destination == config.address) or not config.up;
    if (not forUs)
        return;

    u8 const* payload = p + headerLength;
    u32 payloadLength = total - headerLength;
    switch (p[9]) {
    case PROTO_ICMP: receiveIcmp(source, payload, payloadLength); break;
    case PROTO_UDP: receiveUdp(source, payload, payloadLength); break;
    case PROTO_TCP: receiveTcp(source, destination, payload, payloadLength); break;
    default: break;
    }
}

// MARK: ICMP

constexpr u16 PING_ID = 0xB4A1;

struct Ping {
    bool used = false, done = false;
    u16 sequence = 0;
    u64 sent = 0, rtt = 0;
};
Ping pings[8];
u16 nextPingSequence = 1;

void receiveIcmp(u32 source, u8 const* p, u32 length) {
    if (length < 8 or finish(sum(p, length)) != 0)
        return;
    if (p[0] == 8 and config.up) { // echo request: answer it
        u8 reply[1480];
        if (length > sizeof(reply))
            return;
        memcpy(reply, p, length);
        reply[0] = 0;
        put16(reply + 2, 0);
        put16(reply + 2, finish(sum(reply, length)));
        sendIp(source, PROTO_ICMP, reply, length);
    } else if (p[0] == 0 and get16(p + 4) == PING_ID) { // echo reply to one of ours
        u16 sequence = get16(p + 6);
        for (auto& ping : pings) {
            if (ping.used and not ping.done and ping.sequence == sequence) {
                ping.done = true;
                ping.rtt = now() - ping.sent;
            }
        }
    }
}

// MARK: UDP, DHCP and DNS

constexpr u16 DHCP_CLIENT = 68, DHCP_SERVER = 67, DNS_PORT = 53, DNS_LOCAL = 53535;

void sendUdp(u32 destination, u16 sourcePort, u16 destinationPort, u8 const* data, u32 length) {
    u8 packet[1480];
    if (length + 8 > sizeof(packet))
        return;
    put16(packet, sourcePort);
    put16(packet + 2, destinationPort);
    put16(packet + 4, static_cast<u16>(8 + length));
    put16(packet + 6, 0);
    memcpy(packet + 8, data, length);
    u32 s = pseudoSum(config.address, destination, PROTO_UDP, 8 + length);
    u16 checksum = finish(sum(packet, 8 + length, s));
    put16(packet + 6, checksum ? checksum : 0xFFFF);
    sendIp(destination, PROTO_UDP, packet, 8 + length);
}

// DHCP

enum class DhcpState { Selecting, Requesting, Bound };
DhcpState dhcpState = DhcpState::Selecting;
u32 dhcpXid = 0, dhcpOffered = 0, dhcpServer = 0;
u64 dhcpNextSend = 0;
int dhcpTries = 0;

void dhcpSend(u8 type) {
    u8 m[300] = {};
    m[0] = 1; // request
    m[1] = 1; // Ethernet
    m[2] = 6;
    put32(m + 4, dhcpXid);
    put16(m + 10, 0x8000); // answer by broadcast: we have no address yet
    memcpy(m + 28, card->mac(), 6);
    put32(m + 236, 0x63825363); // magic cookie
    u8* o = m + 240;
    *o++ = 53;
    *o++ = 1;
    *o++ = type;
    if (type == 3) { // request: the offered address, from that server
        *o++ = 50;
        *o++ = 4;
        put32(o, dhcpOffered);
        o += 4;
        *o++ = 54;
        *o++ = 4;
        put32(o, dhcpServer);
        o += 4;
    }
    *o++ = 55; // parameters wanted: netmask, router, DNS, lease time
    *o++ = 4;
    *o++ = 1;
    *o++ = 3;
    *o++ = 6;
    *o++ = 51;
    *o++ = 255;
    sendUdp(BROADCAST, DHCP_CLIENT, DHCP_SERVER, m, sizeof(m));
}

void dhcpStart() {
    dhcpState = DhcpState::Selecting;
    dhcpXid = randomish();
    dhcpTries = 0;
    dhcpSend(1); // discover
    dhcpNextSend = now() + 2000;
}

void receiveDhcp(u8 const* m, u32 length) {
    if (length < 240 or m[0] != 2 or get32(m + 4) != dhcpXid or get32(m + 236) != 0x63825363)
        return;
    u8 type = 0;
    u32 netmask = 0, router = 0, dns = 0, server = 0;
    for (u32 i = 240; i < length;) {
        u8 code = m[i];
        if (code == 255)
            break;
        if (code == 0) {
            i++;
            continue;
        }
        if (i + 1 >= length)
            break;
        u8 len = m[i + 1];
        u8 const* v = m + i + 2;
        if (i + 2 + len > length)
            break;
        if (code == 53 and len >= 1)
            type = v[0];
        else if (code == 1 and len >= 4)
            netmask = get32(v);
        else if (code == 3 and len >= 4)
            router = get32(v);
        else if (code == 6 and len >= 4)
            dns = get32(v);
        else if (code == 54 and len >= 4)
            server = get32(v);
        i += 2 + len;
    }

    if (type == 2 and dhcpState == DhcpState::Selecting) { // offer
        dhcpOffered = get32(m + 16);
        dhcpServer = server;
        dhcpState = DhcpState::Requesting;
        dhcpSend(3);
        dhcpNextSend = now() + 2000;
    } else if (type == 5 and dhcpState == DhcpState::Requesting) { // acknowledged
        config.address = get32(m + 16);
        config.netmask = netmask ? netmask : 0xFFFFFF00;
        config.gateway = router;
        config.dns = dns ? dns : router;
        config.up = true;
        dhcpState = DhcpState::Bound;
        Text<> t;
        t.add("address ");
        logAddress(t, config.address);
        t.add(", gateway ");
        logAddress(t, config.gateway);
        t.add(", dns ");
        logAddress(t, config.dns);
        t.add(" (from DHCP)");
        log(t.cstr());
    } else if (type == 6) { // refused: start over
        dhcpStart();
    }
}

// DNS

struct Query {
    bool used = false, done = false;
    long status = Sys::OK;
    u16 id = 0;
    char name[96];
    u32 address = 0;
    u64 nextSend = 0;
    int tries = 0;
};
Query queries[8];

struct CachedName {
    char name[96];
    u32 address = 0;
    u64 expires = 0;
};
CachedName dnsCache[16];
u32 dnsCacheNext = 0;

bool sameName(char const* a, char const* b) {
    for (; *a and *b; a++, b++) {
        char x = *a >= 'A' and *a <= 'Z' ? static_cast<char>(*a + 32) : *a;
        char y = *b >= 'A' and *b <= 'Z' ? static_cast<char>(*b + 32) : *b;
        if (x != y)
            return false;
    }
    return *a == *b;
}

void copyName(char* dest, char const* src) {
    unsigned long i = 0;
    for (; src[i] and i < 95; i++)
        dest[i] = src[i];
    dest[i] = '\0';
}

void dnsSend(Query& q) {
    u8 m[512] = {};
    put16(m, q.id);
    put16(m + 2, 0x0100); // recursion desired
    put16(m + 4, 1);      // one question
    u32 at = 12;
    char const* label = q.name;
    while (*label) {
        char const* end = label;
        while (*end and *end != '.')
            end++;
        u32 len = static_cast<u32>(end - label);
        if (len == 0 or len > 63 or at + len + 6 > sizeof(m))
            return;
        m[at++] = static_cast<u8>(len);
        memcpy(m + at, label, len);
        at += len;
        label = *end ? end + 1 : end;
    }
    m[at++] = 0;
    put16(m + at, 1); // A record
    put16(m + at + 2, 1); // Internet
    at += 4;
    sendUdp(config.dns, DNS_LOCAL, DNS_PORT, m, at);
    q.tries++;
    q.nextSend = now() + 1000;
}

// Skips a (possibly compressed) name; returns the offset after it, or 0.
u32 skipName(u8 const* m, u32 length, u32 at) {
    while (at < length) {
        u8 len = m[at];
        if (len == 0)
            return at + 1;
        if ((len & 0xC0) == 0xC0)
            return at + 2 <= length ? at + 2 : 0;
        at += 1 + len;
    }
    return 0;
}

void receiveDns(u8 const* m, u32 length) {
    if (length < 12)
        return;
    u16 id = get16(m);
    Query* q = nullptr;
    for (auto& query : queries) {
        if (query.used and not query.done and query.id == id)
            q = &query;
    }
    if (not q)
        return;
    u16 flags = get16(m + 2);
    if (not (flags & 0x8000))
        return;
    q->done = true;
    if ((flags & 0xF) != 0) {
        q->status = Sys::NOT_FOUND; // e.g. no such domain
        return;
    }
    u32 questions = get16(m + 4), answers = get16(m + 6);
    u32 at = 12;
    for (u32 i = 0; i < questions and at; i++)
        at = skipName(m, length, at) ? skipName(m, length, at) + 4 : 0;
    for (u32 i = 0; i < answers and at and at < length; i++) {
        at = skipName(m, length, at);
        if (not at or at + 10 > length)
            break;
        u16 type = get16(m + at), klass = get16(m + at + 2);
        u32 ttl = get32(m + at + 4);
        u16 rdLength = get16(m + at + 8);
        at += 10;
        if (at + rdLength > length)
            break;
        if (type == 1 and klass == 1 and rdLength == 4) {
            q->address = get32(m + at);
            q->status = Sys::OK;
            CachedName& c = dnsCache[dnsCacheNext];
            dnsCacheNext = (dnsCacheNext + 1) % 16;
            copyName(c.name, q->name);
            c.address = q->address;
            c.expires = now() + (ttl < 300 ? ttl : 300) * 1000ul;
            return;
        }
        at += rdLength;
    }
    q->status = Sys::NOT_FOUND;
}

void receiveUdp(u32 source, u8 const* p, u32 length) {
    (void)source;
    if (length < 8)
        return;
    u16 destinationPort = get16(p + 2);
    u32 udpLength = get16(p + 4);
    if (udpLength < 8 or udpLength > length)
        return;
    if (destinationPort == DHCP_CLIENT)
        receiveDhcp(p + 8, udpLength - 8);
    else if (destinationPort == DNS_LOCAL)
        receiveDns(p + 8, udpLength - 8);
}

// MARK: TCP

constexpr u8 FIN = 0x01, SYN = 0x02, RST = 0x04, PSH = 0x08, ACK = 0x10;
constexpr u32 BUFFER = 64 * 1024;
constexpr u32 MAX_IN_FLIGHT_SEGMENTS = 16;

bool before(u32 a, u32 b) { return static_cast<int>(a - b) < 0; }

struct Ring {
    u8* data = nullptr;
    u32 head = 0, size = 0;

    u32 space() const { return BUFFER - size; }

    u32 push(u8 const* in, u32 n) {
        if (n > space())
            n = space();
        for (u32 i = 0; i < n; i++)
            data[(head + size + i) % BUFFER] = in[i];
        size += n;
        return n;
    }

    void peek(u32 offset, u8* out, u32 n) const {
        for (u32 i = 0; i < n; i++)
            out[i] = data[(head + offset + i) % BUFFER];
    }

    void drop(u32 n) {
        if (n > size)
            n = size;
        head = (head + n) % BUFFER;
        size -= n;
    }
};

struct Socket {
    bool used = false;
    bool connected = false; // handshake done
    bool closed = false;    // finished or failed: nothing more will happen
    bool ownerClosed = false;
    bool finSent = false, finAcked = false, finSeqValid = false, peerFin = false;
    long error = Sys::OK;

    u32 remote = 0;
    u16 remotePort = 0, localPort = 0;
    u32 iss = 0, sndUna = 0, sndNxt = 0, sndMax = 0, sndWnd = 0, finSeq = 0;
    u32 rcvNxt = 0, lastWindow = 0;
    u16 mss = 536;

    u64 rtoDeadline = 0, lingerDeadline = 0;
    u32 rto = 1000, retries = 0;

    Ring send, receive;
};

constexpr int MAX_SOCKETS = 32;
Socket sockets[MAX_SOCKETS];
u16 nextLocalPort = 49152;

void release(Socket& s) {
    delete[] s.send.data;
    delete[] s.receive.data;
    s = Socket{};
}

u16 window(Socket const& s) { return static_cast<u16>(s.receive.space() < 65535 ? s.receive.space() : 65535); }

void sendSegment(Socket& s, u8 flags, u32 seq, u8 const* data, u32 length) {
    u8 segment[24 + 1460];
    if (length > 1460)
        return;
    u32 header = (flags & SYN) ? 24 : 20;
    put16(segment, s.localPort);
    put16(segment + 2, s.remotePort);
    put32(segment + 4, seq);
    put32(segment + 8, (flags & ACK) ? s.rcvNxt : 0);
    segment[12] = static_cast<u8>((header / 4) << 4);
    segment[13] = flags;
    s.lastWindow = window(s);
    put16(segment + 14, static_cast<u16>(s.lastWindow));
    put16(segment + 16, 0);
    put16(segment + 18, 0);
    if (flags & SYN) { // maximum segment size option
        segment[20] = 2;
        segment[21] = 4;
        put16(segment + 22, 1460);
    }
    if (length)
        memcpy(segment + header, data, length);
    u32 total = header + length;
    put16(segment + 16, finish(sum(segment, total, pseudoSum(config.address, s.remote, PROTO_TCP, total))));
    sendIp(s.remote, PROTO_TCP, segment, total);
}

void fail(Socket& s, long error) {
    s.closed = true;
    s.error = error;
    s.rtoDeadline = 0;
}

void armTimer(Socket& s) {
    if (not s.rtoDeadline)
        s.rtoDeadline = now() + s.rto;
}

// Sends whatever the window allows, then the FIN once the program closed
// and every byte has gone out.
void transmit(Socket& s) {
    if (not s.connected or s.closed or s.finSent)
        return;
    for (;;) {
        u32 offset = s.sndNxt - s.sndUna; // bytes of the buffer already sent
        if (offset >= s.send.size) {
            if (s.ownerClosed) {
                sendSegment(s, FIN | ACK, s.sndNxt, nullptr, 0);
                if (not s.finSeqValid) {
                    s.finSeq = s.sndNxt;
                    s.finSeqValid = true;
                }
                s.finSent = true;
                s.sndNxt++;
                if (before(s.sndMax, s.sndNxt))
                    s.sndMax = s.sndNxt;
                armTimer(s);
            }
            return;
        }
        u32 limit = s.sndWnd < MAX_IN_FLIGHT_SEGMENTS * s.mss ? s.sndWnd : MAX_IN_FLIGHT_SEGMENTS * s.mss;
        if (limit == 0 and offset == 0)
            limit = 1; // the other side's window is shut: probe it with one byte
        if (offset >= limit)
            return;
        u32 n = s.send.size - offset;
        if (n > limit - offset)
            n = limit - offset;
        if (n > s.mss)
            n = s.mss;
        u8 data[1460];
        s.send.peek(offset, data, n);
        sendSegment(s, ACK | PSH, s.sndNxt, data, n);
        s.sndNxt += n;
        if (before(s.sndMax, s.sndNxt))
            s.sndMax = s.sndNxt;
        armTimer(s);
    }
}

void receiveTcp(u32 source, u32 destination, u8 const* p, u32 length) {
    if (length < 20 or not config.up or destination != config.address)
        return;
    if (finish(sum(p, length, pseudoSum(source, destination, PROTO_TCP, length))) != 0)
        return;
    u16 sourcePort = get16(p), destinationPort = get16(p + 2);
    u32 seq = get32(p + 4), ack = get32(p + 8);
    u32 headerLength = (p[12] >> 4) * 4u;
    u8 flags = p[13];
    u16 peerWindow = get16(p + 14);
    if (headerLength < 20 or headerLength > length)
        return;

    Socket* found = nullptr;
    for (auto& s : sockets) {
        if (s.used and s.localPort == destinationPort and s.remotePort == sourcePort and s.remote == source)
            found = &s;
    }
    if (not found or found->closed)
        return;
    Socket& s = *found;
    u8 const* data = p + headerLength;
    u32 dataLength = length - headerLength;

    if (flags & RST) {
        fail(s, s.connected ? Sys::CONNECTION_RESET : Sys::CONNECTION_REFUSED);
        return;
    }

    if (not s.connected) {
        if ((flags & (SYN | ACK)) != (SYN | ACK) or ack != s.iss + 1)
            return;
        // The maximum segment size the other side accepts.
        for (u32 i = 20; i + 1 < headerLength;) {
            u8 kind = p[i];
            if (kind == 0)
                break;
            if (kind == 1) {
                i++;
                continue;
            }
            u8 len = p[i + 1];
            if (len < 2)
                break;
            if (kind == 2 and len == 4 and i + 4 <= headerLength) {
                u16 mss = get16(p + i + 2);
                s.mss = mss < 1460 ? (mss > 64 ? mss : 64) : 1460;
            }
            i += len;
        }
        s.connected = true;
        s.rcvNxt = seq + 1;
        s.sndUna = s.sndNxt = s.sndMax = ack;
        s.sndWnd = peerWindow;
        s.rtoDeadline = 0;
        s.retries = 0;
        s.rto = 1000;
        sendSegment(s, ACK, s.sndNxt, nullptr, 0);
        transmit(s);
        return;
    }

    if (flags & ACK) {
        if (before(s.sndUna, ack) and not before(s.sndMax, ack)) { // new data acknowledged
            u32 acked = ack - s.sndUna;
            s.send.drop(acked < s.send.size ? acked : s.send.size);
            s.sndUna = ack;
            if (before(s.sndNxt, ack))
                s.sndNxt = ack;
            if (s.finSeqValid and not before(ack, s.finSeq + 1)) {
                s.finAcked = true;
                s.finSent = true;
            }
            s.retries = 0;
            s.rto = 1000;
            s.rtoDeadline = s.sndUna != s.sndMax ? now() + s.rto : 0;
        }
        s.sndWnd = peerWindow;
    }

    bool needAck = false;
    bool fin = flags & FIN;
    if (dataLength > 0 or fin) {
        needAck = true;
        if (not before(s.rcvNxt, seq)) { // starts at or before what we expect
            u32 skip = s.rcvNxt - seq;
            if (skip <= dataLength) {
                u32 take = s.receive.push(data + skip, dataLength - skip);
                s.rcvNxt += take;
                if (fin and skip + take == dataLength and not s.peerFin) {
                    s.rcvNxt++;
                    s.peerFin = true;
                }
            }
        }
        // (out of order: dropped; the duplicate ACK asks for the gap again)
    }
    if (needAck)
        sendSegment(s, ACK, s.sndNxt, nullptr, 0);

    if (s.peerFin and s.finAcked) {
        s.closed = true;
        s.rtoDeadline = 0;
    }
    transmit(s);
}

void tickTcp(u64 t) {
    for (auto& s : sockets) {
        if (not s.used)
            continue;
        if (s.ownerClosed and (s.closed or (s.lingerDeadline and t >= s.lingerDeadline))) {
            if (not s.closed and s.connected)
                sendSegment(s, RST | ACK, s.sndNxt, nullptr, 0);
            release(s);
            continue;
        }
        if (s.closed or not s.rtoDeadline or t < s.rtoDeadline)
            continue;

        if (++s.retries > 8) {
            sendSegment(s, RST, s.sndNxt, nullptr, 0);
            fail(s, Sys::TIMED_OUT);
            continue;
        }
        s.rto = s.rto * 2 < 16000 ? s.rto * 2 : 16000;
        s.rtoDeadline = t + s.rto;
        if (not s.connected) {
            sendSegment(s, SYN, s.iss, nullptr, 0);
        } else {
            // Go back to the oldest unacknowledged byte and send again.
            s.sndNxt = s.sndUna;
            if (not s.finAcked)
                s.finSent = false;
            transmit(s);
        }
    }
}

Socket* socketAt(long socket) {
    if (socket < 0 or socket >= MAX_SOCKETS or not sockets[socket].used)
        return nullptr;
    return &sockets[socket];
}

} // namespace

// MARK: Public interface

void init(E1000& nic) {
    card = &nic;
    dhcpStart();
}

void receiveFrame(u8 const* frame, u32 length) {
    if (length < 14)
        return;
    u16 type = get16(frame + 12);
    if (type == ETHERTYPE_IP)
        receiveIp(frame + 14, length - 14);
    else if (type == ETHERTYPE_ARP)
        receiveArp(frame + 14, length - 14);
}

void tick() {
    u64 t = now();

    if (dhcpState != DhcpState::Bound and t >= dhcpNextSend) {
        if (dhcpState == DhcpState::Requesting and ++dhcpTries < 3) {
            dhcpSend(3);
            dhcpNextSend = t + 2000;
        } else {
            dhcpStart();
        }
    }

    for (auto& w : waiting) {
        if (not w.used)
            continue;
        if (t - w.since > 3000) {
            w.used = false; // nobody answered: drop it (TCP will retry)
        } else if (t - w.lastAsk >= 1000) {
            w.lastAsk = t;
            arpSend(1, BROADCAST_MAC, w.nextHop);
        }
    }

    for (auto& q : queries) {
        if (q.used and not q.done and t >= q.nextSend) {
            if (q.tries >= 4) {
                q.done = true;
                q.status = Sys::TIMED_OUT;
            } else {
                dnsSend(q);
            }
        }
    }

    tickTcp(t);
}

long dnsStart(char const* name) {
    if (not config.up)
        return Sys::NETWORK_DOWN;
    Query* q = nullptr;
    long index = -1;
    for (long i = 0; i < 8; i++) {
        if (not queries[i].used) {
            q = &queries[i];
            index = i;
            break;
        }
    }
    if (not q)
        return Sys::NO_SPACE;
    *q = Query{};
    q->used = true;
    q->id = static_cast<u16>(randomish());
    copyName(q->name, name);

    for (auto& c : dnsCache) {
        if (c.expires > now() and sameName(c.name, name)) {
            q->done = true;
            q->address = c.address;
            return index;
        }
    }
    dnsSend(*q);
    return index;
}

long dnsResult(long query, u32& address) {
    if (query < 0 or query >= 8 or not queries[query].used)
        return Sys::INVALID_ARGUMENT;
    Query& q = queries[query];
    if (not q.done)
        return Sys::WOULD_BLOCK;
    long status = q.status;
    address = q.address;
    q = Query{};
    return status;
}

void dnsCancel(long query) {
    if (query >= 0 and query < 8)
        queries[query] = Query{};
}

long pingStart(u32 address) {
    if (not config.up)
        return Sys::NETWORK_DOWN;
    for (long i = 0; i < 8; i++) {
        Ping& ping = pings[i];
        if (ping.used)
            continue;
        ping = Ping{};
        ping.used = true;
        ping.sequence = nextPingSequence++;
        ping.sent = now();
        u8 m[40] = {};
        m[0] = 8; // echo request
        put16(m + 4, PING_ID);
        put16(m + 6, ping.sequence);
        memcpy(m + 8, "Bhairav OS ping, namaste!", 25);
        put16(m + 2, finish(sum(m, sizeof(m))));
        sendIp(address, PROTO_ICMP, m, sizeof(m));
        return i;
    }
    return Sys::NO_SPACE;
}

long pingResult(long ping, u64& ms) {
    if (ping < 0 or ping >= 8 or not pings[ping].used)
        return Sys::INVALID_ARGUMENT;
    if (not pings[ping].done)
        return Sys::WOULD_BLOCK;
    ms = pings[ping].rtt;
    pings[ping] = Ping{};
    return Sys::OK;
}

void pingCancel(long ping) {
    if (ping >= 0 and ping < 8)
        pings[ping] = Ping{};
}

long tcpOpen(u32 address, u16 port) {
    if (not config.up)
        return Sys::NETWORK_DOWN;
    for (long i = 0; i < MAX_SOCKETS; i++) {
        Socket& s = sockets[i];
        if (s.used)
            continue;
        s = Socket{};
        s.send.data = new u8[BUFFER];
        s.receive.data = new u8[BUFFER];
        if (not s.send.data or not s.receive.data) {
            release(s);
            return Sys::OUT_OF_MEMORY;
        }
        s.used = true;
        s.remote = address;
        s.remotePort = port;
        s.localPort = nextLocalPort;
        nextLocalPort = nextLocalPort == 65535 ? 49152 : static_cast<u16>(nextLocalPort + 1);
        s.iss = randomish();
        s.sndUna = s.iss;
        s.sndNxt = s.sndMax = s.iss + 1;
        sendSegment(s, SYN, s.iss, nullptr, 0);
        armTimer(s);
        return i;
    }
    return Sys::NO_SPACE;
}

TcpState tcpState(long socket, long& error) {
    Socket* s = socketAt(socket);
    if (not s) {
        error = Sys::BAD_HANDLE;
        return TcpState::Closed;
    }
    error = s->error;
    if (s->closed and s->error != Sys::OK)
        return TcpState::Closed;
    return s->connected ? TcpState::Open : TcpState::Connecting;
}

u32 tcpWrite(long socket, u8 const* data, u32 length) {
    Socket* s = socketAt(socket);
    if (not s or s->closed or s->ownerClosed)
        return 0;
    u32 taken = s->send.push(data, length);
    transmit(*s);
    return taken;
}

u32 tcpRead(long socket, u8* out, u32 capacity) {
    Socket* s = socketAt(socket);
    if (not s)
        return 0;
    u32 n = s->receive.size < capacity ? s->receive.size : capacity;
    s->receive.peek(0, out, n);
    s->receive.drop(n);
    // Tell the other side when the window has opened up a lot.
    if (n and s->connected and not s->closed and s->lastWindow < s->receive.space() / 2)
        sendSegment(*s, ACK, s->sndNxt, nullptr, 0);
    return n;
}

bool tcpAtEnd(long socket) {
    Socket* s = socketAt(socket);
    return not s or (s->receive.size == 0 and (s->peerFin or s->closed));
}

void tcpClose(long socket) {
    Socket* s = socketAt(socket);
    if (not s)
        return;
    s->ownerClosed = true;
    s->lingerDeadline = now() + 10000;
    if (not s->connected) {
        release(*s);
        return;
    }
    transmit(*s);
}

} // namespace Net
