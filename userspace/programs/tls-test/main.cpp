// tls-test: checks the cryptography against the published test vectors,
// then makes real HTTPS connections. Runs at every boot.
//
//   handle 1: a connection to the net server

#include <kapala/crypto.h>
#include <kapala/net.h>
#include <kapala/sys.h>
#include <kapala/text.h>
#include <kapala/tls.h>

using namespace Kapala;
namespace C = Kapala::Crypto;

namespace {

bool ok = true;

void fromHex(char const* hex, unsigned char* out) {
    auto nibble = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
    for (int i = 0; hex[2 * i]; i++)
        out[i] = static_cast<unsigned char>(nibble(hex[2 * i]) << 4 | nibble(hex[2 * i + 1]));
}

void check(char const* what, unsigned char const* got, char const* expectedHex) {
    unsigned char expected[128];
    unsigned long n = 0;
    while (expectedHex[n])
        n++;
    fromHex(expectedHex, expected);
    if (not C::equal(got, expected, n / 2)) {
        Text<> t;
        t.add("tls check: FAILED: ").add(what);
        Sys::log(t.cstr());
        ok = false;
    }
}

void knownAnswers() {
    unsigned char out[128];

    C::Sha256::hash("abc", 3, out);
    check("SHA-256", out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // RFC 5869, test case 1
    unsigned char ikm[22], salt[13], info[10], prk[32];
    for (int i = 0; i < 22; i++)
        ikm[i] = 0x0b;
    for (int i = 0; i < 13; i++)
        salt[i] = static_cast<unsigned char>(i);
    for (int i = 0; i < 10; i++)
        info[i] = static_cast<unsigned char>(0xf0 + i);
    C::hkdfExtract(salt, 13, ikm, 22, prk);
    check("HKDF extract", prk, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5");
    C::hkdfExpand(prk, info, 10, out, 42);
    check("HKDF expand", out, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");

    // RFC 7748, 6.1
    unsigned char alice[32];
    fromHex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", alice);
    C::x25519Base(out, alice);
    check("X25519", out, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");

    // RFC 8439, 2.8.2
    unsigned char key[32], nonce[12], aad[12];
    for (int i = 0; i < 32; i++)
        key[i] = static_cast<unsigned char>(0x80 + i);
    fromHex("070000004041424344454647", nonce);
    fromHex("50515253c0c1c2c3c4c5c6c7", aad);
    char const plain[] = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the "
                         "future, sunscreen would be it.";
    unsigned long n = sizeof(plain) - 1;
    static unsigned char sealed[256], opened[256];
    C::chachaPolySeal(key, nonce, aad, 12, reinterpret_cast<unsigned char const*>(plain), n, sealed);
    check("ChaCha20-Poly1305 tag", sealed + n, "1ae10b594f09e26a7e902ecbd0600691");
    check("ChaCha20-Poly1305 data", sealed, "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6");
    if (not C::chachaPolyOpen(key, nonce, aad, 12, sealed, n, opened) or not C::equal(opened, plain, n))
        check("ChaCha20-Poly1305 open", out, "ff");
    sealed[3] ^= 1;
    if (C::chachaPolyOpen(key, nonce, aad, 12, sealed, n, opened)) {
        Sys::log("tls check: FAILED: a tampered ChaCha20-Poly1305 message was accepted");
        ok = false;
    }

    // AES-GCM, test cases 1 and 2 (McGrew and Viega)
    unsigned char zero[16] = {}, gcmNonce[12] = {};
    C::aesGcmSeal(zero, gcmNonce, nullptr, 0, nullptr, 0, out);
    check("AES-128-GCM (empty)", out, "58e2fccefa7e3061367f1d57a4e7455a");
    C::aesGcmSeal(zero, gcmNonce, nullptr, 0, zero, 16, out);
    check("AES-128-GCM", out, "0388dace60b6a392f328c2b971b2fe78ab6e47d42cec13bdf53a67b21257bddf");
}

// Fetches / over HTTPS; returns the status line's code, or a negative error.
long fetch(Net& net, char const* host, Text<200>& detail) {
    unsigned address;
    long r = net.resolve(host, address);
    if (r < 0)
        return r;
    long socket = net.tcpConnect(address, 443);
    if (socket < 0)
        return socket;
    unsigned long start = Sys::timeMs();
    Tls tls;
    r = tls.connect(net, socket, host);
    if (r < 0) {
        detail.add(tls.error());
        tls.close();
        return r;
    }
    unsigned long handshake = Sys::timeMs() - start;
    Text<300> request;
    request.add("GET / HTTP/1.1\r\nHost: ").add(host).add("\r\nConnection: close\r\nUser-Agent: BhairavOS/0.1\r\n\r\n");
    tls.send(request.cstr(), request.length);
    static char page[16 * 1024];
    unsigned long total = 0;
    for (;;) {
        long n = tls.receive(page + total, sizeof(page) - 1 - total, 10000);
        if (n <= 0)
            break;
        total += static_cast<unsigned long>(n);
        if (total >= sizeof(page) - 1)
            break; // enough to see the status
    }
    page[total] = '\0';
    tls.close();
    long status = 0;
    if (total > 12 and page[0] == 'H')
        status = (page[9] - '0') * 100 + (page[10] - '0') * 10 + (page[11] - '0');
    detail.add(tls.cipher()).add(", handshake ").add(handshake).add(" ms, ").add(total).add(" bytes");
    return status;
}

} // namespace

int main() {
    unsigned long start = Sys::timeMs();
    knownAnswers();
    if (ok) {
        Text<> t;
        t.add("tls check: SHA-256, HKDF, X25519, ChaCha20-Poly1305 and AES-128-GCM match the test vectors (")
            .add(Sys::timeMs() - start).add(" ms)");
        Sys::log(t.cstr());
    }

    Net net;
    if (not net.connect(1) or not net.waitUntilUp(15000)) {
        Sys::log("tls check: no network, skipping HTTPS");
        return 0;
    }
    static constexpr char const* HOSTS[] = {"example.com", "www.wikipedia.org"};
    for (char const* host : HOSTS) {
        Text<200> detail;
        long status = fetch(net, host, detail);
        Text<400> t;
        t.add("tls check: https://").add(host).add(" -> ");
        if (status > 0)
            t.add("HTTP ").add(status).add(" (").add(detail.cstr()).add(")");
        else
            t.add("failed, error ").add(status).add(" (").add(detail.cstr()).add(")");
        Sys::log(t.cstr());
    }
    return 0;
}
