#include <kapala/apps.h>
#include <kapala/crypto.h>
#include <kapala/x509.h>

extern "C" int memcmp(void const* a, void const* b, unsigned long n);

namespace Kapala::X509 {

namespace C = Kapala::Crypto;
using u8 = unsigned char;
using u64 = unsigned long;

namespace {

// MARK: DER

struct Der {
    u8 const* p = nullptr;
    u64 n = 0;
};

// Reads one element from `in`: its tag and content (and `whole`: header
// and content). False if it's malformed or runs past the end.
bool read(Der& in, u8& tag, Der& content, Der* whole = nullptr) {
    if (in.n < 2)
        return false;
    u8 const* start = in.p;
    tag = in.p[0];
    u64 length = in.p[1], header = 2;
    if (length & 0x80) {
        u64 bytes = length & 0x7f;
        if (bytes == 0 or bytes > 4 or in.n < 2 + bytes)
            return false;
        length = 0;
        for (u64 i = 0; i < bytes; i++)
            length = length << 8 | in.p[2 + i];
        header += bytes;
    }
    if (length > in.n - header)
        return false;
    content = {in.p + header, length};
    if (whole)
        *whole = {start, header + length};
    in.p += header + length;
    in.n -= header + length;
    return true;
}

bool expect(Der& in, u8 tag, Der& content, Der* whole = nullptr) {
    u8 got;
    return read(in, got, content, whole) and got == tag;
}

bool same(Der a, Der b) { return a.n == b.n and memcmp(a.p, b.p, a.n) == 0; }

bool isOid(Der oid, u8 const* bytes, u64 n) { return oid.n == n and memcmp(oid.p, bytes, n) == 0; }

#define OID(name, ...) constexpr u8 name[] = {__VA_ARGS__}
OID(RSA_KEY, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01);
OID(RSA_SHA256, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b);
OID(RSA_SHA384, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0c);
OID(RSA_SHA512, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0d);
OID(EC_KEY, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01);
OID(P256, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07);
OID(P384, 0x2b, 0x81, 0x04, 0x00, 0x22);
OID(ECDSA_SHA256, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02);
OID(ECDSA_SHA384, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x03);
OID(ECDSA_SHA512, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x04);
OID(ED25519, 0x2b, 0x65, 0x70);
OID(SUBJECT_ALT_NAME, 0x55, 0x1d, 0x11);
OID(BASIC_CONSTRAINTS, 0x55, 0x1d, 0x13);
OID(KEY_USAGE, 0x55, 0x1d, 0x0f);
OID(EXTENDED_KEY_USAGE, 0x55, 0x1d, 0x25);
OID(CERTIFICATE_POLICIES, 0x55, 0x1d, 0x20);
#undef OID

#define IS(oid, NAME) isOid(oid, NAME, sizeof(NAME))

// MARK: Certificates

struct Cert {
    Der tbs;       // the signed part (header included)
    Der sigAlg;    // OID
    Der sigValue;  // the signature (bit string contents after the unused-bits byte)
    Der issuer;    // whole Name, compared byte for byte
    Der subject;
    u64 notBefore = 0, notAfter = 0;
    Der keyAlg;    // OID
    Der keyParam;  // EC: the curve's OID
    Der key;       // bit string contents after the unused-bits byte
    bool ca = false;
    Der names;     // subjectAltName: the GeneralNames' contents
};

// Days since 1970-01-01 (proleptic Gregorian).
long daysFromCivil(long y, long m, long d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

bool readTime(Der& in, u64& out) {
    u8 tag;
    Der t;
    if (not read(in, tag, t))
        return false;
    u64 digitsBefore = tag == 0x17 ? 12 : tag == 0x18 ? 14 : 0; // UTCTime / GeneralizedTime
    if (not digitsBefore or t.n != digitsBefore + 1 or t.p[digitsBefore] != 'Z')
        return false;
    long v[7] = {};
    u64 at = 0;
    auto two = [&](long& field) {
        field = (t.p[at] - '0') * 10 + (t.p[at + 1] - '0');
        at += 2;
    };
    for (u64 i = 0; i < digitsBefore; i++) {
        if (t.p[i] < '0' or t.p[i] > '9')
            return false;
    }
    long year;
    if (tag == 0x17) {
        two(year);
        year += year < 50 ? 2000 : 1900;
    } else {
        long hi, lo;
        two(hi);
        two(lo);
        year = hi * 100 + lo;
    }
    two(v[1]);
    two(v[2]);
    two(v[3]);
    two(v[4]);
    two(v[5]);
    long days = daysFromCivil(year, v[1], v[2]);
    out = static_cast<u64>(days * 86400 + v[3] * 3600 + v[4] * 60 + v[5]);
    return true;
}

bool bitString(Der& in, Der& out) {
    Der bits;
    if (not expect(in, 0x03, bits) or bits.n < 1 or bits.p[0] != 0)
        return false;
    out = {bits.p + 1, bits.n - 1};
    return true;
}

bool parseExtensions(Der list, Cert& c) {
    Der extensions;
    if (not expect(list, 0x30, extensions))
        return false;
    while (extensions.n) {
        Der extension, oid, value;
        if (not expect(extensions, 0x30, extension) or not expect(extension, 0x06, oid))
            return false;
        bool critical = false;
        if (extension.n and extension.p[0] == 0x01) {
            Der flag;
            if (not expect(extension, 0x01, flag) or flag.n != 1)
                return false;
            critical = flag.p[0] != 0;
        }
        if (not expect(extension, 0x04, value))
            return false;
        if (IS(oid, SUBJECT_ALT_NAME)) {
            if (not expect(value, 0x30, c.names))
                return false;
        } else if (IS(oid, BASIC_CONSTRAINTS)) {
            Der seq, flag;
            if (not expect(value, 0x30, seq))
                return false;
            c.ca = seq.n and seq.p[0] == 0x01 and expect(seq, 0x01, flag) and flag.n == 1 and flag.p[0];
        } else if (critical and not IS(oid, KEY_USAGE) and not IS(oid, EXTENDED_KEY_USAGE) and
                   not IS(oid, CERTIFICATE_POLICIES)) {
            return false; // a critical rule we don't understand: refuse rather than ignore it
        }
    }
    return true;
}

bool parse(u8 const* p, u64 n, Cert& c) {
    c = {};
    Der in{p, n}, cert, tbs, alg;
    if (not expect(in, 0x30, cert) or in.n != 0)
        return false;
    if (not expect(cert, 0x30, tbs, &c.tbs) or not expect(cert, 0x30, alg) or not expect(alg, 0x06, c.sigAlg) or
        not bitString(cert, c.sigValue))
        return false;
    Der skip, validity, spki, keyAlg;
    if (tbs.n and tbs.p[0] == 0xa0 and not expect(tbs, 0xa0, skip)) // version
        return false;
    if (not expect(tbs, 0x02, skip) or not expect(tbs, 0x30, skip) or // serial, signature algorithm
        not expect(tbs, 0x30, skip, &c.issuer) or not expect(tbs, 0x30, validity) or
        not readTime(validity, c.notBefore) or not readTime(validity, c.notAfter) or
        not expect(tbs, 0x30, skip, &c.subject) or not expect(tbs, 0x30, spki) or
        not expect(spki, 0x30, keyAlg) or not expect(keyAlg, 0x06, c.keyAlg))
        return false;
    if (keyAlg.n and keyAlg.p[0] == 0x06 and not expect(keyAlg, 0x06, c.keyParam))
        return false;
    if (not bitString(spki, c.key))
        return false;
    while (tbs.n) { // issuer/subject unique IDs, then extensions [3]
        u8 tag;
        Der field;
        if (not read(tbs, tag, field))
            return false;
        if (tag == 0xa3 and not parseExtensions(field, c))
            return false;
    }
    return true;
}

// MARK: Signatures

enum class Kind { RsaPkcs1, RsaPss, Ecdsa, Ed25519 };

u64 digest(C::Hash hash, Der data, u8 out[64]) {
    if (hash == C::Hash::Sha256) {
        C::Sha256::hash(data.p, data.n, out);
        return 32;
    }
    C::Sha512 h(hash == C::Hash::Sha384);
    h.update(data.p, data.n);
    h.finish(out);
    return hash == C::Hash::Sha384 ? 48 : 64;
}

Der stripZero(Der integer) {
    while (integer.n > 1 and integer.p[0] == 0) {
        integer.p++;
        integer.n--;
    }
    return integer;
}

// Checks `signature` over `data` with the key in `signer`. `curveBits`, if
// not 0, is the curve an ECDSA key must be on (TLS ties the scheme to it).
bool verifyWith(Cert const& signer, Kind kind, C::Hash hash, Der data, Der signature, int curveBits = 0) {
    u8 h[64];
    if (kind == Kind::Ed25519)
        return IS(signer.keyAlg, ED25519) and signer.key.n == 32 and signature.n == 64 and
               C::ed25519Verify(signature.p, data.p, data.n, signer.key.p);
    u64 hLength = digest(hash, data, h);
    if (kind == Kind::Ecdsa) {
        if (not IS(signer.keyAlg, EC_KEY))
            return false;
        int bits = IS(signer.keyParam, P256) ? 256 : IS(signer.keyParam, P384) ? 384 : 0;
        if (not bits or (curveBits and bits != curveBits))
            return false;
        Der sig = signature, seq, r, s;
        if (not expect(sig, 0x30, seq) or not expect(seq, 0x02, r) or not expect(seq, 0x02, s))
            return false;
        r = stripZero(r);
        s = stripZero(s);
        return C::ecdsaVerify(bits, signer.key.p, signer.key.n, r.p, r.n, s.p, s.n, h, hLength);
    }
    if (not IS(signer.keyAlg, RSA_KEY))
        return false;
    Der key = signer.key, seq, n, e;
    if (not expect(key, 0x30, seq) or not expect(seq, 0x02, n) or not expect(seq, 0x02, e))
        return false;
    n = stripZero(n);
    e = stripZero(e);
    return kind == Kind::RsaPkcs1 ? C::rsaPkcs1Verify(n.p, n.n, e.p, e.n, signature.p, signature.n, hash, h)
                                  : C::rsaPssVerify(n.p, n.n, e.p, e.n, signature.p, signature.n, hash, h);
}

// Whether `issuer` signed `cert`.
bool signedBy(Cert const& cert, Cert const& issuer) {
    Der alg = cert.sigAlg;
    if (IS(alg, RSA_SHA256))
        return verifyWith(issuer, Kind::RsaPkcs1, C::Hash::Sha256, cert.tbs, cert.sigValue);
    if (IS(alg, RSA_SHA384))
        return verifyWith(issuer, Kind::RsaPkcs1, C::Hash::Sha384, cert.tbs, cert.sigValue);
    if (IS(alg, RSA_SHA512))
        return verifyWith(issuer, Kind::RsaPkcs1, C::Hash::Sha512, cert.tbs, cert.sigValue);
    if (IS(alg, ECDSA_SHA256))
        return verifyWith(issuer, Kind::Ecdsa, C::Hash::Sha256, cert.tbs, cert.sigValue);
    if (IS(alg, ECDSA_SHA384))
        return verifyWith(issuer, Kind::Ecdsa, C::Hash::Sha384, cert.tbs, cert.sigValue);
    if (IS(alg, ECDSA_SHA512))
        return verifyWith(issuer, Kind::Ecdsa, C::Hash::Sha512, cert.tbs, cert.sigValue);
    if (IS(alg, ED25519))
        return verifyWith(issuer, Kind::Ed25519, C::Hash::Sha512, cert.tbs, cert.sigValue);
    return false; // an algorithm we don't check
}

// MARK: Names

char lower(char ch) { return ch >= 'A' and ch <= 'Z' ? static_cast<char>(ch + 32) : ch; }

bool equalIgnoringCase(char const* a, u64 aLength, char const* b, u64 bLength) {
    if (aLength != bLength)
        return false;
    for (u64 i = 0; i < aLength; i++) {
        if (lower(a[i]) != lower(b[i]))
            return false;
    }
    return true;
}

bool nameMatches(char const* pattern, u64 patternLength, char const* host) {
    u64 hostLength = 0;
    while (host[hostLength])
        hostLength++;
    if (patternLength > 2 and pattern[0] == '*' and pattern[1] == '.') {
        // "*.example.com": exactly one more label in front
        u64 dot = 0;
        while (dot < hostLength and host[dot] != '.')
            dot++;
        return dot > 0 and dot < hostLength and
               equalIgnoringCase(pattern + 1, patternLength - 1, host + dot, hostLength - dot);
    }
    return equalIgnoringCase(pattern, patternLength, host, hostLength);
}

bool namesHost(Cert const& cert, char const* host) {
    Der names = cert.names;
    while (names.n) {
        u8 tag;
        Der name;
        if (not read(names, tag, name))
            return false;
        if (tag == 0x82 and nameMatches(reinterpret_cast<char const*>(name.p), name.n, host)) // dNSName
            return true;
    }
    return false;
}

} // namespace

char const* verifyChain(Chain const& chain, char const* host, unsigned long now, unsigned char const* roots) {
    if (chain.count == 0 or chain.count > MAX_CHAIN)
        return "the server sent no certificate";
    static Cert certs[MAX_CHAIN];
    for (u64 i = 0; i < chain.count; i++) {
        if (not parse(chain.certs[i], chain.lengths[i], certs[i]))
            return "a certificate couldn't be read (or has a critical rule we don't know)";
    }
    if (not namesHost(certs[0], host))
        return "the certificate is for another site";

    bool used[MAX_CHAIN] = {true};
    Cert const* current = &certs[0];
    for (u64 depth = 0; depth <= MAX_CHAIN; depth++) {
        if (now < current->notBefore or now > current->notAfter)
            return depth ? "a certificate in the chain has expired (or isn't valid yet)"
                         : "the certificate has expired (or isn't valid yet; is the clock right?)";
        if (depth > 0 and not current->ca)
            return "a certificate in the chain isn't allowed to sign others";

        // Signed by a trusted root?
        bool trusted = false;
        forEachInCatalog(roots, [&](u8 const* data, u64 size) {
            static Cert root;
            if (not trusted and parse(data, size, root) and same(root.subject, current->issuer) and
                signedBy(*current, root))
                trusted = true;
        });
        if (trusted)
            return nullptr;

        // Otherwise by the next certificate the server sent.
        Cert const* next = nullptr;
        for (u64 j = 1; j < chain.count and not next; j++) {
            if (not used[j] and same(certs[j].subject, current->issuer) and signedBy(*current, certs[j])) {
                used[j] = true;
                next = &certs[j];
            }
        }
        if (not next)
            return "the certificate isn't from an authority this system trusts";
        current = next;
    }
    return "the certificate chain is too long";
}

char const* verifyHandshakeSignature(unsigned char const* leaf, unsigned long leafLength, unsigned scheme,
                                     unsigned char const* signature, unsigned long signatureLength,
                                     unsigned char const* content, unsigned long contentLength) {
    static Cert cert;
    if (not parse(leaf, leafLength, cert))
        return "the certificate couldn't be read";
    Der data{content, contentLength}, sig{signature, signatureLength};
    bool good;
    switch (scheme) {
    case 0x0403: good = verifyWith(cert, Kind::Ecdsa, C::Hash::Sha256, data, sig, 256); break;
    case 0x0503: good = verifyWith(cert, Kind::Ecdsa, C::Hash::Sha384, data, sig, 384); break;
    case 0x0804: good = verifyWith(cert, Kind::RsaPss, C::Hash::Sha256, data, sig); break;
    case 0x0805: good = verifyWith(cert, Kind::RsaPss, C::Hash::Sha384, data, sig); break;
    case 0x0806: good = verifyWith(cert, Kind::RsaPss, C::Hash::Sha512, data, sig); break;
    case 0x0807: good = verifyWith(cert, Kind::Ed25519, C::Hash::Sha512, data, sig); break;
    default: return "the server signed with a scheme we don't check";
    }
    return good ? nullptr : "the server's signature doesn't match its certificate";
}

} // namespace Kapala::X509
