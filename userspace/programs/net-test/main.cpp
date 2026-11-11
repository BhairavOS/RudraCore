// net-test: checks the network at every boot.
//
//   handle 1: a connection to the net server
//
// Waits for DHCP, pings the gateway, looks up a name and fetches a web page
// over TCP. Without internet access (only the later steps need it) it says
// how far it got.

#include <kapala/net.h>
#include <kapala/sys.h>
#include <kapala/text.h>

using namespace Kapala;

namespace {

bool startsWith(char const* text, char const* prefix) {
    while (*prefix) {
        if (*text++ != *prefix++)
            return false;
    }
    return true;
}

void report(char const* what, long result) {
    Text<> t;
    t.add("net check: ").add(what).add(" failed (error ").add(result).add(")");
    Sys::log(t.cstr());
}

} // namespace

int main() {
    Net net;
    if (not net.connect(1)) {
        Sys::log("net check: skipped, no network server");
        return 0;
    }
    unsigned long start = Sys::timeMs();
    if (not net.waitUntilUp(10000)) {
        Sys::log("net check: no address from DHCP after 10 s (is there a network card?)");
        return 0;
    }
    unsigned long dhcpMs = Sys::timeMs() - start;

    NetStatus status{};
    net.status(status);
    long rtt = net.ping(status.gateway);
    if (rtt < 0)
        report("ping of the gateway", rtt);

    unsigned address = 0;
    start = Sys::timeMs();
    long result = net.resolve("example.com", address);
    unsigned long dnsMs = Sys::timeMs() - start;
    if (result < 0) {
        report("DNS lookup of example.com (no internet?)", result);
        return 0;
    }

    start = Sys::timeMs();
    long socket = net.tcpConnect(address, 80);
    if (socket < 0) {
        report("TCP connection to example.com:80", socket);
        return 0;
    }
    char const request[] = "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n"
                           "User-Agent: BhairavOS/0.1\r\n\r\n";
    net.send(socket, request, sizeof(request) - 1);

    static char page[64 * 1024];
    unsigned long total = 0;
    for (;;) {
        long n = net.receive(socket, page + total, sizeof(page) - 1 - total, 10000);
        if (n <= 0) {
            if (n < 0)
                report("receiving", n);
            break;
        }
        total += static_cast<unsigned long>(n);
        if (total >= sizeof(page) - 1)
            break;
    }
    net.close(socket);
    unsigned long httpMs = Sys::timeMs() - start;
    page[total] = '\0';

    Text<> t;
    t.add("net check: DHCP in ").add(dhcpMs).add(" ms, gateway ping ");
    if (rtt >= 0)
        t.add(rtt).add(" ms");
    else
        t.add("failed");
    t.add(", example.com is ");
    Net::format(address, t);
    t.add(" (").add(dnsMs).add(" ms), HTTP: ").add(total).add(" bytes in ").add(httpMs).add(" ms");
    Sys::log(t.cstr());
    if (not startsWith(page, "HTTP/1.1 200") and not startsWith(page, "HTTP/1.0 200"))
        Sys::log("net check: FAILED: the web page didn't come back with 200 OK");
    return 0;
}
