#include <kapala/net.h>

extern "C" void* memcpy(void* dest, void const* src, unsigned long n);

namespace Kapala {

namespace Proto = Protocol::Net;

bool Net::connect(Sys::Handle channel) {
    _channel = Channel(channel);
    long memory = Sys::memoryCreate(Proto::BUFFER_SIZE);
    if (memory < 0)
        return false;
    auto* buffer = static_cast<unsigned char*>(Sys::memoryMap(memory));
    if (not buffer) {
        Sys::close(memory);
        return false;
    }
    Proto::Request request{};
    request.op = Proto::Op::Attach;
    Proto::Reply reply{};
    if (_channel.send(request, memory) != Sys::OK or _channel.recv(reply) != Sys::OK or reply.status != Sys::OK) {
        Sys::memoryUnmap(buffer);
        return false;
    }
    _buffer = buffer;
    return true;
}

long Net::call(Proto::Request& request, Proto::Reply& reply) {
    if (not _buffer)
        return Sys::NETWORK_DOWN;
    long result = _channel.send(request);
    if (result == Sys::OK)
        result = _channel.recv(reply);
    return result == Sys::OK ? reply.status : result;
}

long Net::status(NetStatus& out) {
    Proto::Request request{};
    request.op = Proto::Op::Status;
    Proto::Reply reply{};
    long result = call(request, reply);
    if (result < 0)
        return result;
    out.up = reply.up;
    out.address = reply.address;
    out.netmask = reply.netmask;
    out.gateway = reply.gateway;
    out.dns = reply.dns;
    for (int i = 0; i < 6; i++)
        out.mac[i] = reply.mac[i];
    return Sys::OK;
}

bool Net::waitUntilUp(unsigned timeoutMs) {
    unsigned long deadline = Sys::timeMs() + timeoutMs;
    for (;;) {
        NetStatus s{};
        if (status(s) < 0)
            return false;
        if (s.up)
            return true;
        if (Sys::timeMs() >= deadline)
            return false;
        Sys::sleep(100);
    }
}

bool Net::parse(char const* text, unsigned& address) {
    unsigned result = 0;
    for (int part = 0; part < 4; part++) {
        if (*text < '0' or *text > '9')
            return false;
        unsigned value = 0;
        int digits = 0;
        while (*text >= '0' and *text <= '9') {
            value = value * 10 + static_cast<unsigned>(*text++ - '0');
            if (++digits > 3 or value > 255)
                return false;
        }
        result = result << 8 | value;
        if (part < 3 and *text++ != '.')
            return false;
    }
    if (*text)
        return false;
    address = result;
    return true;
}

long Net::resolve(char const* host, unsigned& address, unsigned timeoutMs) {
    if (parse(host, address))
        return Sys::OK;
    Proto::Request request{};
    request.op = Proto::Op::Resolve;
    request.timeoutMs = timeoutMs;
    unsigned long n = 0;
    for (; host[n]; n++) {
        if (n + 1 >= Proto::MAX_NAME)
            return Sys::INVALID_ARGUMENT;
        request.name[n] = host[n];
    }
    Proto::Reply reply{};
    long result = call(request, reply);
    if (result == Sys::OK)
        address = reply.address;
    return result;
}

long Net::ping(unsigned address, unsigned timeoutMs) {
    Proto::Request request{};
    request.op = Proto::Op::Ping;
    request.address = address;
    request.timeoutMs = timeoutMs;
    Proto::Reply reply{};
    long result = call(request, reply);
    return result < 0 ? result : static_cast<long>(reply.value);
}

long Net::tcpConnect(unsigned address, unsigned short port, unsigned timeoutMs) {
    Proto::Request request{};
    request.op = Proto::Op::TcpConnect;
    request.address = address;
    request.port = port;
    request.timeoutMs = timeoutMs;
    Proto::Reply reply{};
    long result = call(request, reply);
    return result < 0 ? result : static_cast<long>(reply.socket);
}

long Net::send(long socket, void const* data, unsigned long length) {
    if (not _buffer)
        return Sys::NETWORK_DOWN;
    auto const* in = static_cast<unsigned char const*>(data);
    unsigned long total = 0;
    while (total < length) {
        unsigned long chunk = length - total < Proto::BUFFER_SIZE ? length - total : Proto::BUFFER_SIZE;
        memcpy(_buffer, in + total, chunk);
        Proto::Request request{};
        request.op = Proto::Op::Send;
        request.socket = static_cast<unsigned>(socket);
        request.length = static_cast<unsigned>(chunk);
        request.timeoutMs = 30000;
        Proto::Reply reply{};
        long result = call(request, reply);
        if (result < 0)
            return result;
        total += reply.length;
    }
    return static_cast<long>(total);
}

long Net::receive(long socket, void* data, unsigned long capacity, unsigned timeoutMs) {
    Proto::Request request{};
    request.op = Proto::Op::Receive;
    request.socket = static_cast<unsigned>(socket);
    request.length = static_cast<unsigned>(capacity < Proto::BUFFER_SIZE ? capacity : Proto::BUFFER_SIZE);
    request.timeoutMs = timeoutMs;
    Proto::Reply reply{};
    long result = call(request, reply);
    if (result < 0)
        return result;
    memcpy(data, _buffer, reply.length);
    return reply.length;
}

long Net::close(long socket) {
    Proto::Request request{};
    request.op = Proto::Op::Close;
    request.socket = static_cast<unsigned>(socket);
    Proto::Reply reply{};
    return call(request, reply);
}

} // namespace Kapala
