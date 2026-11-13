#pragma once

// Kapalaframe: TLS 1.3 client connections (RFC 8446), for HTTPS.
//
//   long socket = net.tcpConnect(address, 443);
//   Tls tls;
//   if (tls.connect(net, socket, "example.com") == Sys::OK) {
//       tls.send(request, length);
//       long n = tls.receive(buffer, sizeof(buffer)); // 0: the server closed
//   }
//   tls.close();
//
// Key exchange: X25519. Ciphers: ChaCha20-Poly1305 or AES-128-GCM, with
// SHA-256. The connection is encrypted and protected against tampering,
// but the server's certificate is NOT checked yet: it could be an
// impostor (a "man in the middle"). verified() says so.

#include <kapala/crypto.h>
#include <kapala/net.h>
#include <kapala/x509.h>

namespace Kapala {

class Tls {
    using u8 = unsigned char;

public:
    Tls() = default;
    Tls(Tls const&) = delete;
    Tls& operator=(Tls const&) = delete;
    ~Tls();

    // The handshake, over a connected TCP socket. The server must prove it's
    // `host`: a certificate chain to a trusted root (roots.bin) naming it,
    // valid now, and a CertificateVerify signature from that certificate's
    // key. OK, or an error (error() then describes it).
    long connect(Net& net, long socket, char const* host);

    // Sends all of it.
    long send(void const* data, unsigned long length);

    // Up to `capacity` bytes; 0 once the server has closed.
    long receive(void* data, unsigned long capacity, unsigned timeoutMs = 30000);

    // Says goodbye (close_notify) and closes the socket.
    void close();

    char const* cipher() const { return _cipher; }
    char const* error() const { return _error; }
    bool verified() const { return _verified; } // the server proved it's the host

    // The certificates the server sent (until this object is destroyed).
    X509::Chain const& chain() const { return _chain; }

private:
    struct Direction {
        u8 key[32];
        u8 iv[12];
        unsigned long sequence = 0;
    };

    long fail(char const* why, long code);
    long readRecord(u8& type, u8*& data, unsigned long& length, unsigned timeoutMs);
    long sendRecord(u8 type, u8 const* data, unsigned long length);
    void setKeys(Direction& d, u8 const secret[32]);
    bool decrypt(Direction& d, u8 const* header, u8* data, unsigned long& length, u8& type);
    long nextHandshake(u8& type, u8*& body, unsigned long& length);
    long checkCertificates(u8 const* message, unsigned long length, char const* host);

    Net* _net = nullptr;
    long _socket = -1;
    bool _aes = false;     // AES-128-GCM, else ChaCha20-Poly1305
    bool _encrypted = false;
    bool _closed = false;
    Direction _read, _write;
    u8 _readSecret[32];

    // Received bytes not yet made into records.
    u8* _in = nullptr;
    unsigned long _inLength = 0, _inConsumed = 0;
    // Decrypted application data not yet returned.
    u8* _plain = nullptr;
    unsigned long _plainStart = 0, _plainLength = 0;
    // Handshake messages being collected.
    u8* _handshake = nullptr;
    unsigned long _handshakeLength = 0, _handshakeConsumed = 0;

    char const* _cipher = "";
    char const* _error = "";
    bool _verified = false;
    u8* _chainData = nullptr; // a copy of the Certificate message
    X509::Chain _chain;
};

} // namespace Kapala
