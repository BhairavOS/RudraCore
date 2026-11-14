// tls-test: checks the cryptography against the published test vectors,
// then makes real HTTPS connections. Runs at every boot.
//
//   handle 1: a connection to the net server

#include <kapala/apps.h>
#include <kapala/crypto.h>
#include <kapala/http.h>
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

    // FIPS 180-4 examples
    C::Sha512::hash("abc", 3, out);
    check("SHA-512", out, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                          "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    C::Sha512 sha384(true);
    sha384.update("abc", 3);
    sha384.finish(out);
    check("SHA-384", out, "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
                          "8086072ba1e7cc2358baeca134c825a7");

    // RFC 8032, 7.1, tests 1-3; then the same with one bit changed
    struct Vector {
        char const* key;
        char const* message;
        char const* signature;
    };
    static constexpr Vector ED25519[] = {
        {"d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
        {"3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
        {"fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
         "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
    };
    for (Vector const& v : ED25519) {
        unsigned char pk[32], sig[64], msg[2];
        fromHex(v.key, pk);
        fromHex(v.signature, sig);
        fromHex(v.message, msg);
        unsigned long len = 0;
        while (v.message[2 * len])
            len++;
        bool good = C::ed25519Verify(sig, msg, len, pk);
        sig[10] ^= 1;
        bool tampered = C::ed25519Verify(sig, msg, len, pk);
        if (not good or tampered) {
            Sys::log(good ? "tls check: FAILED: Ed25519 accepted a changed signature"
                          : "tls check: FAILED: Ed25519 rejected an RFC 8032 signature");
            ok = false;
        }
    }

    // The App Store's built-in packages are signed by a trusted key; a
    // single changed byte must make one fail.
    bool any = false;
    forEachInCatalog(static_cast<unsigned char const*>(Sys::openFile("catalog")),
                     [&](unsigned char const* data, unsigned long size) {
        char const* why = "";
        any = true;
        if (not verifyPackage(data, size, why)) {
            Text<> t;
            t.add("tls check: FAILED: a built-in package doesn't verify: ").add(why);
            Sys::log(t.cstr());
            ok = false;
            return;
        }
        static unsigned char copy[MAX_APP_SIZE / 8];
        if (size > sizeof(copy))
            return;
        for (unsigned long i = 0; i < size; i++)
            copy[i] = data[i];
        copy[size / 2] ^= 1;
        if (verifyPackage(copy, size, why)) {
            Sys::log("tls check: FAILED: a changed package still verified");
            ok = false;
        }
    });
    if (not any) {
        Sys::log("tls check: FAILED: no packages in the catalog to check");
        ok = false;
    }
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
    detail.add(tls.verified() ? "certificate verified, " : "NOT verified, ").add(tls.cipher()).add(", handshake ").add(handshake).add(" ms, ").add(total).add(" bytes");
    return status;
}

// Certificate checks that must fail: a real server under the wrong name,
// and its real chain checked at the wrong time, against no roots, and with
// one byte changed.
void certificateFailures(Net& net) {
    unsigned address;
    if (net.resolve("example.com", address) < 0)
        return;
    bool good = true;
    auto attempt = [&](char const* host, Tls& tls) {
        long socket = net.tcpConnect(address, 443);
        return socket >= 0 and tls.connect(net, socket, host) == Sys::OK;
    };
    {
        Tls wrong;
        if (attempt("wrong-name.invalid", wrong)) {
            Sys::log("tls check: FAILED: a server was accepted under another site's name");
            good = false;
        }
        wrong.close();
    }
    Tls tls;
    if (not attempt("example.com", tls) or not tls.verified()) {
        Sys::log("tls check: FAILED: example.com couldn't be verified for the negative checks");
        ok = false;
        return;
    }
    X509::Chain chain = tls.chain();
    auto const* roots = static_cast<unsigned char const*>(Sys::openFile("roots"));
    Sys::SystemInfo info{};
    Sys::systemInfo(info);
    unsigned char const noRoots[12] = {'B', 'C', 'T', '1', 0, 0, 0, 0, 12, 0, 0, 0};
    static unsigned char changed[8192];
    char const* inFuture = X509::verifyChain(chain, "example.com", 4102444800, roots); // 2100
    char const* untrusted = X509::verifyChain(chain, "example.com", info.unixTime, noRoots);
    char const* tampered = "too big to copy";
    if (chain.lengths[0] <= sizeof(changed)) {
        for (unsigned long i = 0; i < chain.lengths[0]; i++)
            changed[i] = chain.certs[0][i];
        changed[chain.lengths[0] - 10] ^= 1; // inside the signature
        X509::Chain bad = chain;
        bad.certs[0] = changed;
        tampered = X509::verifyChain(bad, "example.com", info.unixTime, roots);
    }
    if (not inFuture or not untrusted or not tampered or chain.lengths[0] > sizeof(changed)) {
        Sys::log("tls check: FAILED: an expired, untrusted or changed chain was accepted");
        good = false;
    }
    tls.close();
    if (good)
        Sys::log("tls check: refused a wrong host name, an expired chain, unknown roots and a changed certificate");
    ok = ok and good;
}

// TLS 1.3 negative tests, against scripts/tls-negative.sh on the build
// machine (only when it runs: it serves its test authority over HTTP).
// Each port is one scenario; only the control may succeed.
void negativeTests(Net& net) {
    unsigned char* ca = nullptr;
    unsigned long caSize = 0;
    char const* why = "";
    if (httpGet(net, "http://10.0.2.2:8080/tls/ca.bin", ca, caSize, why, 64 * 1024) < 0) {
        Sys::log("tls negative check: skipped (scripts/tls-negative.sh isn't running)");
        return;
    }
    struct Scenario {
        unsigned short port;
        char const* name;
        bool succeeds;
        bool download; // also fetch a page (truncation checks go through httpGet)
    };
    static constexpr Scenario SCENARIOS[] = {
        {8443, "control: a good server", true, false},
        {8444, "wrong host name", false, false},
        {8445, "expired certificate", false, false},
        {8446, "self-signed certificate", false, false},
        {8447, "TLS 1.2 only", false, false},
        {8448, "certificate for client authentication only (EKU)", false, false},
        {8449, "intermediate that isn't a CA", false, false},
        {8450, "1024-bit RSA key", false, false},
        {8451, "no key exchange group in common (P-256 only)", false, false},
        {8452, "asks for a client certificate", false, false},
        {8453, "only a cipher we don't offer", false, false},
        {8454, "a byte flipped in ServerHello", false, false},
        {8455, "a byte flipped in an encrypted record", false, false},
        {8456, "a record header changed", false, false},
        {8457, "plaintext handshake injected after ServerHello", false, false},
        {8458, "response cut short without close_notify", false, true},
        {8459, "control: the same page in full", true, true},
    };
    unsigned address = 0;
    net.resolve("10.0.2.2", address);
    int passed = 0, total = 0;
    for (Scenario const& s : SCENARIOS) {
        bool succeeded;
        Text<120> detail;
        if (s.download) {
            long socket = net.tcpConnect(address, s.port);
            Tls tls;
            tls.useRoots(ca);
            succeeded = socket >= 0 and tls.connect(net, socket, "bhairav.test") == Sys::OK;
            if (succeeded) {
                char const request[] = "GET / HTTP/1.1\r\nHost: bhairav.test\r\nConnection: close\r\n\r\n";
                tls.send(request, sizeof(request) - 1);
                static char page[8192];
                unsigned long got = 0;
                for (long n; (n = tls.receive(page + got, sizeof(page) - got, 5000)) > 0;)
                    got += static_cast<unsigned long>(n);
                succeeded = tls.closedCleanly(); // a cut-off response must not look complete
                detail.add(got).add(" bytes, ").add(tls.closedCleanly() ? "close_notify" : "no close_notify");
            } else {
                detail.add(tls.error());
            }
            tls.close();
        } else {
            long socket = net.tcpConnect(address, s.port);
            Tls tls;
            tls.useRoots(ca);
            succeeded = socket >= 0 and tls.connect(net, socket, "bhairav.test") == Sys::OK;
            detail.add(succeeded ? "accepted" : tls.error());
            tls.close();
        }
        total++;
        bool right = succeeded == s.succeeds;
        passed += right;
        if (not right) {
            Text<300> t;
            t.add("tls negative check: FAILED: ").add(s.name).add(s.succeeds ? " was refused: " : " was accepted: ")
                .add(detail.cstr());
            Sys::log(t.cstr());
            ok = false;
        } else if (not s.succeeds) {
            Text<300> t;
            t.add("tls negative check: refused ").add(s.name).add(" (").add(detail.cstr()).add(")");
            Sys::log(t.cstr());
        }
    }
    delete[] ca;
    Text<120> t;
    t.add("tls negative check: ").add(passed).add(" of ").add(total).add(" scenarios behaved correctly");
    Sys::log(t.cstr());
}

} // namespace

int main() {
    unsigned long start = Sys::timeMs();
    knownAnswers();
    if (ok) {
        Text<> t;
        t.add("tls check: SHA-256/384/512, HKDF, X25519, Ed25519, ChaCha20-Poly1305, AES-128-GCM and package signatures check out (")
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

    certificateFailures(net);
    negativeTests(net);

    // The download helper apps use (redirects, chunked bodies, TLS).
    unsigned char* body = nullptr;
    unsigned long size = 0;
    char const* why = "";
    long r = httpGet(net, "https://example.com/", body, size, why);
    bool found = false;
    for (unsigned long i = 0; r == Sys::OK and i + 14 <= size and not found; i++)
        found = C::equal(body + i, "Example Domain", 14);
    Text<200> t;
    if (found)
        t.add("tls check: httpGet downloaded https://example.com/ (").add(size).add(" bytes)");
    else
        t.add("tls check: FAILED: httpGet (").add(why).add(")");
    Sys::log(t.cstr());
    delete[] body;
    return 0;
}
