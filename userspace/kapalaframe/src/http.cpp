#include <kapala/http.h>
#include <kapala/text.h>
#include <kapala/tls.h>

extern "C" {
void* memcpy(void* dest, void const* src, unsigned long n);
void* memmove(void* dest, void const* src, unsigned long n);
}

namespace Kapala {

namespace {

using u8 = unsigned char;
using u64 = unsigned long;

char lower(char c) { return c >= 'A' and c <= 'Z' ? static_cast<char>(c + 32) : c; }

bool startsWith(char const* text, char const* prefix) {
    for (; *prefix; text++, prefix++) {
        if (lower(*text) != lower(*prefix))
            return false;
    }
    return true;
}

struct Address {
    bool secure = false;
    char host[128] = {};
    unsigned short port = 80;
    char path[512] = "/";
};

bool parseAddress(char const* text, Address& out) {
    out = Address{};
    if (startsWith(text, "https://")) {
        out.secure = true;
        out.port = 443;
        text += 8;
    } else if (startsWith(text, "http://")) {
        text += 7;
    } else {
        return false;
    }
    u64 n = 0;
    while (*text and *text != '/' and *text != ':' and n + 1 < sizeof(out.host))
        out.host[n++] = lower(*text++);
    out.host[n] = '\0';
    if (*text == ':') {
        unsigned port = 0;
        for (text++; *text >= '0' and *text <= '9'; text++)
            port = port * 10 + static_cast<unsigned>(*text - '0');
        out.port = static_cast<unsigned short>(port);
    }
    if (*text == '/') {
        n = 0;
        while (*text and *text != '#' and *text != ' ' and n + 1 < sizeof(out.path))
            out.path[n++] = *text++;
        out.path[n] = '\0';
    }
    return n > 0 or out.host[0];
}

int hexDigit(char c) {
    c = lower(c);
    return c >= '0' and c <= '9' ? c - '0' : c >= 'a' and c <= 'f' ? c - 'a' + 10 : -1;
}

// One request; fills status, location and body.
long fetchOnce(Net& net, Address const& a, int& status, char* location, u64 locationCapacity, u8*& body,
               u64& bodyLength, char const*& error, u64 limit) {
    unsigned ip;
    long r = net.resolve(a.host, ip);
    if (r < 0) {
        error = "the site's name couldn't be found";
        return r;
    }
    long socket = net.tcpConnect(ip, a.port);
    if (socket < 0) {
        error = "couldn't connect to the site";
        return socket;
    }
    Tls tls;
    if (a.secure and tls.connect(net, socket, a.host) < 0) {
        error = tls.error();
        tls.close();
        return Sys::CONNECTION_RESET;
    }
    Text<800> request;
    request.add("GET ").add(a.path).add(" HTTP/1.1\r\nHost: ").add(a.host)
        .add("\r\nUser-Agent: BhairavOS/0.1\r\nAccept: */*\r\nConnection: close\r\n\r\n");
    if (a.secure)
        tls.send(request.cstr(), request.length);
    else
        net.send(socket, request.cstr(), request.length);

    u64 capacity = 64 * 1024, used = 0;
    u8* raw = new u8[capacity];
    for (;;) {
        if (used == capacity) {
            if (capacity >= limit + 64 * 1024) {
                delete[] raw;
                if (a.secure)
                    tls.close();
                else
                    net.close(socket);
                error = "the file is too big";
                return Sys::NO_SPACE;
            }
            u8* bigger = new u8[capacity * 2];
            memcpy(bigger, raw, used);
            delete[] raw;
            raw = bigger;
            capacity *= 2;
        }
        long n = a.secure ? tls.receive(raw + used, capacity - used, 20000)
                          : net.receive(socket, raw + used, capacity - used, 20000);
        if (n < 0) { // a failure isn't the end of the body
            if (a.secure)
                tls.close();
            else
                net.close(socket);
            delete[] raw;
            error = a.secure and tls.error()[0] ? tls.error() : "the download was cut short";
            return n;
        }
        if (n == 0)
            break;
        used += static_cast<u64>(n);
    }
    bool clean = not a.secure or tls.closedCleanly(); // (plain HTTP can't tell)
    if (a.secure)
        tls.close();
    else
        net.close(socket);

    // "HTTP/1.1 200 OK", headers, a blank line, the body.
    u64 headerEnd = 0;
    for (u64 i = 0; i + 3 < used; i++) {
        if (raw[i] == '\r' and raw[i + 1] == '\n' and raw[i + 2] == '\r' and raw[i + 3] == '\n') {
            headerEnd = i + 4;
            break;
        }
    }
    if (used < 12 or not headerEnd or not startsWith(reinterpret_cast<char const*>(raw), "HTTP/")) {
        delete[] raw;
        error = "the server's answer wasn't HTTP";
        return Sys::INVALID_ARGUMENT;
    }
    status = (raw[9] - '0') * 100 + (raw[10] - '0') * 10 + (raw[11] - '0');
    bool chunked = false, haveLength = false;
    u64 contentLength = 0;
    location[0] = '\0';
    for (u64 i = 0; i < headerEnd;) {
        u64 end = i;
        while (end < headerEnd and raw[end] != '\n')
            end++;
        char const* line = reinterpret_cast<char const*>(raw + i);
        u64 lineLength = end - i;
        if (startsWith(line, "Location:")) {
            u64 s = 9;
            while (s < lineLength and line[s] == ' ')
                s++;
            u64 n = 0;
            while (s < lineLength and line[s] != '\r' and n + 1 < locationCapacity)
                location[n++] = line[s++];
            location[n] = '\0';
        } else if (startsWith(line, "Transfer-Encoding:") and lineLength > 25) {
            for (u64 s = 18; s + 7 <= lineLength; s++)
                chunked = chunked or startsWith(line + s, "chunked");
        } else if (startsWith(line, "Content-Length:")) {
            u64 s = 15;
            while (s < lineLength and line[s] == ' ')
                s++;
            for (; s < lineLength and line[s] >= '0' and line[s] <= '9' and contentLength < (1ul << 40); s++) {
                contentLength = contentLength * 10 + static_cast<u64>(line[s] - '0');
                haveLength = true;
            }
        }
        i = end + 1;
    }

    // Truncation: the body must be complete by its own measure (chunked:
    // the final empty chunk; Content-Length: that many bytes); otherwise,
    // over TLS, the server must have ended with close_notify.
    u8* p = raw + headerEnd;
    u64 n = used - headerEnd;
    bool complete = haveLength and not chunked ? n >= contentLength : clean;
    if (haveLength and not chunked and n > contentLength)
        n = contentLength;
    if (chunked) { // decode in place
        u64 in = 0, out = 0;
        complete = false;
        while (in < n) {
            u64 size = 0;
            int d, digits = 0;
            while (in < n and (d = hexDigit(static_cast<char>(p[in]))) >= 0) {
                size = size * 16 + static_cast<u64>(d);
                in++;
                digits++;
            }
            while (in < n and p[in] != '\n')
                in++;
            in++;
            if (digits and size == 0) {
                complete = true; // the last chunk
                break;
            }
            if (not digits or in > n or size > n - in)
                break; // cut short (complete stays false)
            memmove(p + out, p + in, size);
            out += size;
            in += size + 2;
        }
        n = out;
    }
    if (not complete) {
        delete[] raw;
        error = "the download was cut short (or the server closed without saying so)";
        return Sys::CONNECTION_RESET;
    }
    body = new u8[n ? n : 1];
    memcpy(body, p, n);
    bodyLength = n;
    delete[] raw;
    return Sys::OK;
}

} // namespace

long httpGet(Net& net, char const* address, u8*& data, u64& length, char const*& error, u64 limit) {
    char current[600];
    u64 n = 0;
    for (; address[n] and n + 1 < sizeof(current); n++)
        current[n] = address[n];
    current[n] = '\0';

    for (int redirects = 0; redirects < 6; redirects++) {
        Address a;
        if (not parseAddress(current, a)) {
            error = "not an http:// or https:// address";
            return Sys::INVALID_ARGUMENT;
        }
        int status = 0;
        char location[600];
        u8* body = nullptr;
        u64 bodyLength = 0;
        long r = fetchOnce(net, a, status, location, sizeof(location), body, bodyLength, error, limit);
        if (r < 0)
            return r;
        if (status >= 300 and status < 400 and location[0]) {
            delete[] body;
            if (location[0] == '/') { // same site
                Text<600> next;
                next.add(a.secure ? "https://" : "http://").add(a.host).add(location);
                for (n = 0; next.cstr()[n] and n + 1 < sizeof(current); n++)
                    current[n] = next.cstr()[n];
                current[n] = '\0';
            } else {
                for (n = 0; location[n] and n + 1 < sizeof(current); n++)
                    current[n] = location[n];
                current[n] = '\0';
            }
            continue;
        }
        if (status != 200) {
            delete[] body;
            error = status == 404 ? "the file isn't there (404)" : "the server refused (not 200 OK)";
            return Sys::NOT_FOUND;
        }
        data = body;
        length = bodyLength;
        return Sys::OK;
    }
    error = "too many redirects";
    return Sys::INVALID_ARGUMENT;
}

} // namespace Kapala
