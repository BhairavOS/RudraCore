#include "http.h"

#include <kapala/text.h>

namespace Browser {

namespace Sys = Kapala::Sys;

namespace {

constexpr u64 MAX_RESPONSE = 4 * 1024 * 1024;

// The header's value if `line` is "Name: value".
char const* headerValue(char const* line, char const* name) {
    if (not startsWith(line, name))
        return nullptr;
    line += length(name);
    if (*line != ':')
        return nullptr;
    line++;
    while (*line == ' ' or *line == '\t')
        line++;
    return line;
}

int hexDigit(char c) {
    if (c >= '0' and c <= '9')
        return c - '0';
    c = lower(c);
    if (c >= 'a' and c <= 'f')
        return c - 'a' + 10;
    return -1;
}

// Decodes a chunked body in place; returns its new length.
u64 dechunk(char* data, u64 n) {
    u64 in = 0, out = 0;
    while (in < n) {
        u64 size = 0;
        int digits = 0;
        int d;
        while (in < n and (d = hexDigit(data[in])) >= 0) {
            size = size * 16 + static_cast<u64>(d);
            in++;
            digits++;
        }
        while (in < n and data[in] != '\n') // chunk extensions, then CRLF
            in++;
        in++;
        if (digits == 0 or size == 0)
            break;
        if (size > n - in)
            size = n - in;
        memcpy(data + out, data + in, size);
        out += size;
        in += size;
        while (in < n and (data[in] == '\r' or data[in] == '\n'))
            in++;
    }
    return out;
}

} // namespace

void fetch(Kapala::Net& net, Url const& url, Response& out, Progress progress) {
    out.error = Sys::OK;
    out.status = 0;
    out.location[0] = out.contentType[0] = out.security[0] = '\0';
    out.body.clear();

    Kapala::Text<160> message;
    message.add("Looking up ").add(url.host).add("...");
    progress(message.cstr());
    unsigned address = 0;
    long result = net.resolve(url.host, address);
    if (result < 0) {
        out.error = result;
        return;
    }

    message = {};
    message.add("Connecting to ").add(url.host).add("...");
    progress(message.cstr());
    long socket = net.tcpConnect(address, url.port);
    if (socket < 0) {
        out.error = socket;
        return;
    }

    // https: everything goes through TLS.
    Kapala::Tls* tls = nullptr;
    if (url.isHttps()) {
        progress("Securing the connection (TLS)...");
        tls = new Kapala::Tls();
        long r = tls->connect(net, socket, url.host);
        if (r < 0) {
            copy(out.security, tls->error(), sizeof(out.security));
            tls->close();
            delete tls;
            out.error = Sys::INVALID_ARGUMENT;
            return;
        }
        copy(out.security, tls->cipher(), sizeof(out.security));
    }
    auto sendAll = [&](char const* data, u64 n) {
        return tls ? tls->send(data, n) : net.send(socket, data, n);
    };
    auto receive = [&](char* data, u64 capacity) {
        return tls ? tls->receive(data, capacity, 20000) : net.receive(socket, data, capacity, 20000);
    };
    auto finish = [&]() {
        if (tls) {
            tls->close();
            delete tls;
        } else {
            net.close(socket);
        }
    };

    char request[Url::MAX_PATH + Url::MAX_HOST + 256];
    copy(request, "GET ", sizeof(request));
    append(request, url.path, sizeof(request));
    append(request, " HTTP/1.1\r\nHost: ", sizeof(request));
    append(request, url.host, sizeof(request));
    append(request,
           "\r\nUser-Agent: BhairavBrowser/0.1 (Bhairav OS)\r\n"
           "Accept: text/html, text/plain;q=0.9, */*;q=0.1\r\n"
           "Accept-Language: en\r\nConnection: close\r\n\r\n",
           sizeof(request));
    result = sendAll(request, length(request));
    if (result < 0) {
        finish();
        out.error = result;
        return;
    }

    // Read until the server closes the connection.
    Vector<char> raw;
    static char chunk[16 * 1024];
    u64 lastReport = 0;
    for (;;) {
        long n = receive(chunk, sizeof(chunk));
        if (n < 0) {
            if (raw.empty()) {
                out.error = n;
                finish();
                return;
            }
            break; // keep what arrived
        }
        if (n == 0)
            break;
        if (raw.size() + static_cast<u64>(n) > MAX_RESPONSE)
            break;
        raw.append(chunk, static_cast<u64>(n));
        if (raw.size() - lastReport >= 16 * 1024) {
            lastReport = raw.size();
            message = {};
            message.add("Receiving ").add(url.host).add(": ").add(raw.size() / 1024).add(" KB");
            progress(message.cstr());
        }
    }
    finish();
    raw.push('\0');

    // Status line: "HTTP/1.1 200 OK"
    char* text = raw.data();
    if (not startsWith(text, "HTTP/")) {
        out.error = Sys::INVALID_ARGUMENT;
        return;
    }
    char* p = text;
    while (*p and *p != ' ')
        p++;
    while (*p == ' ')
        p++;
    while (*p >= '0' and *p <= '9')
        out.status = out.status * 10 + (*p++ - '0');

    // Headers, one per line, up to an empty line.
    bool chunked = false;
    char* cursor = p;
    while (*cursor and *cursor != '\n')
        cursor++;
    char* bodyStart = nullptr;
    while (*cursor == '\n') {
        char* line = cursor + 1;
        char* end = line;
        while (*end and *end != '\n')
            end++;
        bool more = *end == '\n';
        *end = '\0';
        if (end > line and end[-1] == '\r')
            end[-1] = '\0';
        if (line[0] == '\0') { // the empty line: the body follows
            bodyStart = more ? end + 1 : end;
            break;
        }
        if (char const* v = headerValue(line, "Location"))
            copy(out.location, v, sizeof(out.location));
        else if (char const* v = headerValue(line, "Content-Type"))
            copy(out.contentType, v, sizeof(out.contentType));
        else if (char const* v = headerValue(line, "Transfer-Encoding"))
            chunked = startsWith(v, "chunked");
        if (not more)
            break;
        *end = '\n';
        cursor = end;
    }
    if (not bodyStart) {
        out.body.push('\0');
        return;
    }

    u64 bodyLength = raw.size() - 1 - static_cast<u64>(bodyStart - text);
    if (chunked)
        bodyLength = dechunk(bodyStart, bodyLength);
    out.body.append(bodyStart, bodyLength);
    out.body.push('\0');
}

} // namespace Browser
