#pragma once

// Kapalaframe: checking a server's certificates (X.509, RFC 5280), so TLS
// knows it reached the real site and not someone in between.

namespace Kapala::X509 {

constexpr unsigned long MAX_CHAIN = 8;

// The certificates a server sent, its own first (DER, as in TLS).
struct Chain {
    unsigned char const* certs[MAX_CHAIN];
    unsigned long lengths[MAX_CHAIN];
    unsigned long count = 0;
};

// Checks a server's chain at time `now` (seconds since 1970, UTC). Returns
// nullptr if it's good, or why not:
//   - the first certificate names `host` (subjectAltName DNS names; "*."
//     covers exactly one label)
//   - each certificate is within its dates, and is signed by the next one
//     (which must be a CA) or by a root in `roots`
//   - `roots` is roots.bin from the boot image: a catalog (kapala/apps.h
//     format) of trusted DER certificates
// Signatures: RSA PKCS #1 (SHA-256/384/512), ECDSA P-256/P-384, Ed25519.
char const* verifyChain(Chain const& chain, char const* host, unsigned long now, unsigned char const* roots);

// Checks a TLS 1.3 CertificateVerify signature (`scheme` from the
// handshake) of `content` with the key in the leaf certificate. nullptr if good.
char const* verifyHandshakeSignature(unsigned char const* leaf, unsigned long leafLength, unsigned scheme,
                                     unsigned char const* signature, unsigned long signatureLength,
                                     unsigned char const* content, unsigned long contentLength);

} // namespace Kapala::X509
