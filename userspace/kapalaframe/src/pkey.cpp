// Public-key signature checking for certificates: RSA (PKCS #1 v1.5 and
// PSS, RFC 8017) and ECDSA on P-256 and P-384 (FIPS 186-4). Only checking:
// everything here works on public data, so none of it needs to run in
// constant time.

#include <kapala/crypto.h>

extern "C" {
void* memcpy(void* dest, void const* src, unsigned long n);
void* memset(void* dest, int value, unsigned long n);
int memcmp(void const* a, void const* b, unsigned long n);
}

namespace Kapala::Crypto {

namespace {

constexpr int MAX_LIMBS = 4096 / 32 + 1; // RSA up to 4096 bits

// Numbers are little-endian arrays of 32-bit limbs; each Modulus says how many.
using Limbs = u32[MAX_LIMBS];

// Big-endian bytes -> k limbs. False if it doesn't fit.
bool fromBytes(u32* out, int k, u8 const* p, u64 n) {
    while (n and *p == 0) {
        p++;
        n--;
    }
    if (n > static_cast<u64>(4 * k))
        return false;
    memset(out, 0, 4 * static_cast<u64>(k));
    for (u64 i = 0; i < n; i++)
        out[i / 4] |= u32{p[n - 1 - i]} << (8 * (i % 4));
    return true;
}

void toBytes(u8* p, u64 n, u32 const* a, int k) {
    for (u64 i = 0; i < n; i++)
        p[n - 1 - i] = i / 4 < static_cast<u64>(k) ? static_cast<u8>(a[i / 4] >> (8 * (i % 4))) : 0;
}

int compare(u32 const* a, u32 const* b, int k) {
    for (int i = k - 1; i >= 0; i--) {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

bool isZero(u32 const* a, int k) {
    for (int i = 0; i < k; i++) {
        if (a[i])
            return false;
    }
    return true;
}

// a -= b; returns the borrow.
u32 subtract(u32* a, u32 const* b, int k) {
    u64 borrow = 0;
    for (int i = 0; i < k; i++) {
        u64 d = u64{a[i]} - b[i] - borrow;
        a[i] = static_cast<u32>(d);
        borrow = (d >> 32) & 1;
    }
    return static_cast<u32>(borrow);
}

// a += b; returns the carry.
u32 addTo(u32* a, u32 const* b, int k) {
    u64 c = 0;
    for (int i = 0; i < k; i++) {
        u64 s = u64{a[i]} + b[i] + c;
        a[i] = static_cast<u32>(s);
        c = s >> 32;
    }
    return static_cast<u32>(c);
}

// Arithmetic modulo an odd number, in Montgomery form (a * 2^(32k) mod m).
struct Modulus {
    int k = 0;
    Limbs m{};
    Limbs r2{}; // 2^(64k) mod m: converts into Montgomery form
    Limbs one{}; // 1 in Montgomery form
    u32 inv = 0; // -m^-1 mod 2^32

    bool init(u8 const* p, u64 n) {
        k = static_cast<int>((n + 3) / 4);
        if (k > MAX_LIMBS or not fromBytes(m, k, p, n) or not(m[0] & 1))
            return false;
        while (k > 1 and m[k - 1] == 0)
            k--;
        u32 x = 1;
        for (int i = 0; i < 5; i++)
            x *= 2 - m[0] * x; // Newton: x = m^-1 mod 2^32
        inv = static_cast<u32>(0 - x);
        // r2 = 2^(64k) mod m, by doubling 1 that many times.
        memset(r2, 0, sizeof(r2));
        r2[0] = 1;
        for (int i = 0; i < 64 * k; i++) {
            u32 top = r2[k - 1] >> 31;
            for (int j = k - 1; j > 0; j--)
                r2[j] = r2[j] << 1 | r2[j - 1] >> 31;
            r2[0] <<= 1;
            if (top or compare(r2, m, k) >= 0)
                subtract(r2, m, k);
        }
        Limbs plain{};
        plain[0] = 1;
        mul(one, plain, r2);
        return true;
    }

    // o = a * b / 2^(32k) mod m (CIOS). o may alias a or b.
    void mul(u32* o, u32 const* a, u32 const* b) const {
        u32 t[MAX_LIMBS + 2] = {};
        for (int i = 0; i < k; i++) {
            u64 c = 0;
            for (int j = 0; j < k; j++) {
                u64 s = u64{t[j]} + u64{a[j]} * b[i] + c;
                t[j] = static_cast<u32>(s);
                c = s >> 32;
            }
            u64 s = u64{t[k]} + c;
            t[k] = static_cast<u32>(s);
            t[k + 1] = static_cast<u32>(s >> 32);
            u32 q = t[0] * inv;
            c = (u64{t[0]} + u64{q} * m[0]) >> 32;
            for (int j = 1; j < k; j++) {
                s = u64{t[j]} + u64{q} * m[j] + c;
                t[j - 1] = static_cast<u32>(s);
                c = s >> 32;
            }
            s = u64{t[k]} + c;
            t[k - 1] = static_cast<u32>(s);
            t[k] = t[k + 1] + static_cast<u32>(s >> 32);
        }
        if (t[k] or compare(t, m, k) >= 0)
            subtract(t, m, k);
        memcpy(o, t, 4 * static_cast<u64>(k));
    }

    void toMont(u32* o, u32 const* a) const { mul(o, a, r2); }

    void fromMont(u32* o, u32 const* a) const {
        Limbs plain{};
        plain[0] = 1;
        mul(o, a, plain);
    }

    void add(u32* o, u32 const* a, u32 const* b) const {
        Limbs t;
        memcpy(t, a, 4 * static_cast<u64>(k));
        if (addTo(t, b, k) or compare(t, m, k) >= 0)
            subtract(t, m, k);
        memcpy(o, t, 4 * static_cast<u64>(k));
    }

    void sub(u32* o, u32 const* a, u32 const* b) const {
        Limbs t;
        memcpy(t, a, 4 * static_cast<u64>(k));
        if (subtract(t, b, k))
            addTo(t, m, k);
        memcpy(o, t, 4 * static_cast<u64>(k));
    }

    // o = base ^ exponent (base in Montgomery form, exponent big-endian bytes).
    void power(u32* o, u32 const* base, u8 const* exponent, u64 n) const {
        Limbs r;
        memcpy(r, one, sizeof(r));
        for (u64 i = 0; i < n; i++) {
            for (int bit = 7; bit >= 0; bit--) {
                mul(r, r, r);
                if ((exponent[i] >> bit) & 1)
                    mul(r, r, base);
            }
        }
        memcpy(o, r, 4 * static_cast<u64>(k));
    }

    // o = a^-1 (m prime), via Fermat: a^(m-2).
    void inverse(u32* o, u32 const* a) const {
        Limbs e;
        memcpy(e, m, sizeof(e));
        Limbs two{};
        two[0] = 2;
        subtract(e, two, k);
        u8 bytes[4 * MAX_LIMBS];
        toBytes(bytes, 4 * static_cast<u64>(k), e, k);
        power(o, a, bytes, 4 * static_cast<u64>(k));
    }
};

// MARK: Hashes

u64 hashSize(Hash h) { return h == Hash::Sha256 ? 32 : h == Hash::Sha384 ? 48 : 64; }

void hashOf(Hash h, void const* a, u64 aLength, void const* b, u64 bLength, u8* out) {
    if (h == Hash::Sha256) {
        Sha256 s;
        s.update(a, aLength);
        s.update(b, bLength);
        s.finish(out);
    } else {
        Sha512 s(h == Hash::Sha384);
        s.update(a, aLength);
        s.update(b, bLength);
        s.finish(out);
    }
}

// MARK: RSA

// m = signature ^ e mod n, as big-endian bytes of n's length. False if the
// key or signature is malformed.
bool rsaOpen(u8 const* n, u64 nLength, u8 const* e, u64 eLength, u8 const* signature, u64 signatureLength,
             u8* out, u64& outLength) {
    static Modulus mod; // big: keep it off the stack
    while (nLength and *n == 0) {
        n++;
        nLength--;
    }
    if (nLength < 128 or nLength > 512 or eLength == 0 or eLength > 8 or signatureLength != nLength or
        not mod.init(n, nLength))
        return false;
    Limbs s;
    if (not fromBytes(s, mod.k, signature, signatureLength) or compare(s, mod.m, mod.k) >= 0)
        return false;
    mod.toMont(s, s);
    mod.power(s, s, e, eLength);
    mod.fromMont(s, s);
    toBytes(out, nLength, s, mod.k);
    outLength = nLength;
    return true;
}

} // namespace

bool rsaPkcs1Verify(u8 const* n, u64 nLength, u8 const* e, u64 eLength, u8 const* signature, u64 signatureLength,
                    Hash hash, u8 const* digest) {
    static constexpr u8 PREFIX_256[] = {0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                        0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};
    static constexpr u8 PREFIX_384[] = {0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                        0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30};
    static constexpr u8 PREFIX_512[] = {0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                        0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40};
    u8 const* prefix = hash == Hash::Sha256 ? PREFIX_256 : hash == Hash::Sha384 ? PREFIX_384 : PREFIX_512;
    u64 h = hashSize(hash), t = sizeof(PREFIX_256) + h;
    static u8 em[512];
    u64 k = 0;
    if (not rsaOpen(n, nLength, e, eLength, signature, signatureLength, em, k) or k < t + 11)
        return false;
    // 00 01 FF..FF 00 DigestInfo
    if (em[0] != 0 or em[1] != 1 or em[k - t - 1] != 0)
        return false;
    for (u64 i = 2; i < k - t - 1; i++) {
        if (em[i] != 0xff)
            return false;
    }
    return memcmp(em + k - t, prefix, sizeof(PREFIX_256)) == 0 and memcmp(em + k - h, digest, h) == 0;
}

bool rsaPssVerify(u8 const* n, u64 nLength, u8 const* e, u64 eLength, u8 const* signature, u64 signatureLength,
                  Hash hash, u8 const* digest) {
    static u8 em[512];
    u64 k = 0;
    if (not rsaOpen(n, nLength, e, eLength, signature, signatureLength, em, k))
        return false;
    // emBits = modBits - 1: when modBits is 8k+1 the first byte is all padding.
    while (*n == 0)
        n++;
    u64 bits = 8 * k;
    for (u8 top = n[0]; not(top & 0x80); top = static_cast<u8>(top << 1))
        bits--;
    u64 emBits = bits - 1, emLength = (emBits + 7) / 8;
    u8 const* m = em + (k - emLength);
    u64 h = hashSize(hash);
    if (emLength < 2 * h + 2 or m[emLength - 1] != 0xbc)
        return false;
    u64 dbLength = emLength - h - 1;
    u8 const* hashed = m + dbLength;
    u8 topMask = static_cast<u8>(0xff >> (8 * emLength - emBits));
    if (m[0] & ~topMask)
        return false;
    // DB = maskedDB xor MGF1(H)
    static u8 db[512];
    for (u64 done = 0, counter = 0; done < dbLength; counter++) {
        u8 c[4] = {static_cast<u8>(counter >> 24), static_cast<u8>(counter >> 16), static_cast<u8>(counter >> 8),
                   static_cast<u8>(counter)};
        u8 block[64];
        hashOf(hash, hashed, h, c, 4, block);
        for (u64 i = 0; i < h and done < dbLength; i++, done++)
            db[done] = m[done] ^ block[i];
    }
    db[0] &= topMask;
    // DB = 00..00 01 salt, with a salt as long as the hash (as TLS 1.3 requires)
    u64 padding = dbLength - h - 1;
    for (u64 i = 0; i < padding; i++) {
        if (db[i])
            return false;
    }
    if (db[padding] != 1)
        return false;
    u8 prefix[8 + 64] = {};
    memcpy(prefix + 8, digest, h);
    u8 expected[64];
    hashOf(hash, prefix, 8 + h, db + padding + 1, h, expected);
    return memcmp(expected, hashed, h) == 0;
}

// MARK: ECDSA

namespace {

struct Curve {
    u64 size; // bytes
    char const* p;
    char const* b;
    char const* n;
    char const* gx;
    char const* gy;
};

constexpr Curve P256 = {
    32,
    "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff",
    "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b",
    "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",
    "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
    "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5",
};

constexpr Curve P384 = {
    48,
    "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff",
    "b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef",
    "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973",
    "aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7",
    "3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f",
};

void hexBytes(char const* hex, u8* out, u64 n) {
    auto nibble = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
    for (u64 i = 0; i < n; i++)
        out[i] = static_cast<u8>(nibble(hex[2 * i]) << 4 | nibble(hex[2 * i + 1]));
}

// A point in Jacobian coordinates (x/z^2, y/z^3), in Montgomery form; z = 0 is infinity.
struct Point {
    Limbs x, y, z;
};

struct Group {
    Modulus field;
    Limbs b;

    void dbl(Point& r, Point const& p) const {
        int k = field.k;
        if (isZero(p.z, k) or isZero(p.y, k)) {
            memset(&r, 0, sizeof(r));
            return;
        }
        Limbs delta, gamma, beta, alpha, t, u;
        field.mul(delta, p.z, p.z);
        field.mul(gamma, p.y, p.y);
        field.mul(beta, p.x, gamma);
        field.sub(t, p.x, delta);
        field.add(u, p.x, delta);
        field.mul(alpha, t, u);
        field.add(t, alpha, alpha);
        field.add(alpha, t, alpha); // 3 (x - delta)(x + delta), as a = -3
        Limbs z3;
        field.add(t, p.y, p.z);
        field.mul(t, t, t);
        field.sub(t, t, gamma);
        field.sub(z3, t, delta);
        Limbs beta4, beta8, x3;
        field.add(beta4, beta, beta);
        field.add(beta4, beta4, beta4);
        field.add(beta8, beta4, beta4);
        field.mul(x3, alpha, alpha);
        field.sub(x3, x3, beta8);
        Limbs y3, g2;
        field.sub(t, beta4, x3);
        field.mul(y3, alpha, t);
        field.mul(g2, gamma, gamma);
        field.add(g2, g2, g2);
        field.add(g2, g2, g2);
        field.add(g2, g2, g2);
        field.sub(y3, y3, g2);
        memcpy(r.x, x3, sizeof(x3));
        memcpy(r.y, y3, sizeof(y3));
        memcpy(r.z, z3, sizeof(z3));
    }

    void add(Point& r, Point const& p, Point const& q) const {
        int k = field.k;
        if (isZero(p.z, k)) {
            r = q;
            return;
        }
        if (isZero(q.z, k)) {
            r = p;
            return;
        }
        Limbs z1z1, z2z2, u1, u2, s1, s2, t;
        field.mul(z1z1, p.z, p.z);
        field.mul(z2z2, q.z, q.z);
        field.mul(u1, p.x, z2z2);
        field.mul(u2, q.x, z1z1);
        field.mul(t, q.z, z2z2);
        field.mul(s1, p.y, t);
        field.mul(t, p.z, z1z1);
        field.mul(s2, q.y, t);
        if (compare(u1, u2, k) == 0) {
            if (compare(s1, s2, k) != 0) {
                memset(&r, 0, sizeof(r));
                return;
            }
            dbl(r, p);
            return;
        }
        Limbs h, rr, h2, h3, u1h2, x3, y3, z3;
        field.sub(h, u2, u1);
        field.sub(rr, s2, s1);
        field.mul(h2, h, h);
        field.mul(h3, h, h2);
        field.mul(u1h2, u1, h2);
        field.mul(x3, rr, rr);
        field.sub(x3, x3, h3);
        field.sub(x3, x3, u1h2);
        field.sub(x3, x3, u1h2);
        field.sub(t, u1h2, x3);
        field.mul(y3, rr, t);
        field.mul(t, s1, h3);
        field.sub(y3, y3, t);
        field.mul(z3, p.z, q.z);
        field.mul(z3, z3, h);
        memcpy(r.x, x3, sizeof(x3));
        memcpy(r.y, y3, sizeof(y3));
        memcpy(r.z, z3, sizeof(z3));
    }

    // y^2 == x^3 - 3x + b (affine, Montgomery form)
    bool onCurve(u32 const* x, u32 const* y) const {
        Limbs l, r, t;
        field.mul(l, y, y);
        field.mul(r, x, x);
        field.mul(r, r, x);
        field.add(t, x, x);
        field.add(t, t, x);
        field.sub(r, r, t);
        field.add(r, r, b);
        return compare(l, r, field.k) == 0;
    }
};

} // namespace

bool ecdsaVerify(int curveBits, u8 const* point, u64 pointLength, u8 const* r, u64 rLength, u8 const* s, u64 sLength,
                 u8 const* digest, u64 digestLength) {
    Curve const* curve = curveBits == 256 ? &P256 : curveBits == 384 ? &P384 : nullptr;
    if (not curve)
        return false;
    u64 size = curve->size;
    if (pointLength != 1 + 2 * size or point[0] != 4) // uncompressed only
        return false;

    static Group g;
    static Modulus order;
    u8 bytes[48];
    hexBytes(curve->p, bytes, size);
    g.field.init(bytes, size);
    hexBytes(curve->n, bytes, size);
    order.init(bytes, size);
    int k = g.field.k;

    // r and s in [1, n - 1]
    Limbs rr, ss;
    if (not fromBytes(rr, k, r, rLength) or not fromBytes(ss, k, s, sLength) or isZero(rr, k) or isZero(ss, k) or
        compare(rr, order.m, k) >= 0 or compare(ss, order.m, k) >= 0)
        return false;

    // e: the digest's leftmost bits (as many as the order has), reduced
    Limbs e;
    fromBytes(e, k, digest, digestLength < size ? digestLength : size);
    if (compare(e, order.m, k) >= 0)
        subtract(e, order.m, k);

    // w = s^-1; u1 = e w; u2 = r w (mod n)
    Limbs w, u1, u2, t;
    order.toMont(t, ss);
    order.inverse(w, t);
    order.toMont(t, e);
    order.mul(u1, t, w);
    order.fromMont(u1, u1);
    order.toMont(t, rr);
    order.mul(u2, t, w);
    order.fromMont(u2, u2);

    // The public key, checked to be on the curve, and the generator.
    Point q{}, gen{};
    Limbs plain;
    hexBytes(curve->b, bytes, size);
    fromBytes(plain, k, bytes, size);
    g.field.toMont(g.b, plain);
    if (not fromBytes(plain, k, point + 1, size) or compare(plain, g.field.m, k) >= 0)
        return false;
    g.field.toMont(q.x, plain);
    if (not fromBytes(plain, k, point + 1 + size, size) or compare(plain, g.field.m, k) >= 0)
        return false;
    g.field.toMont(q.y, plain);
    memcpy(q.z, g.field.one, sizeof(q.z));
    if (not g.onCurve(q.x, q.y))
        return false;
    hexBytes(curve->gx, bytes, size);
    fromBytes(plain, k, bytes, size);
    g.field.toMont(gen.x, plain);
    hexBytes(curve->gy, bytes, size);
    fromBytes(plain, k, bytes, size);
    g.field.toMont(gen.y, plain);
    memcpy(gen.z, g.field.one, sizeof(gen.z));

    // u1 G + u2 Q, both at once (Shamir's trick)
    Point both, acc{};
    g.add(both, gen, q);
    for (int bit = 32 * k - 1; bit >= 0; bit--) {
        g.dbl(acc, acc);
        int a = (u1[bit / 32] >> (bit % 32)) & 1, c = (u2[bit / 32] >> (bit % 32)) & 1;
        if (a and c)
            g.add(acc, acc, both);
        else if (a)
            g.add(acc, acc, gen);
        else if (c)
            g.add(acc, acc, q);
    }
    if (isZero(acc.z, k))
        return false;

    // x = X / Z^2, then compare x mod n with r
    Limbs z2, zi, x;
    g.field.mul(z2, acc.z, acc.z);
    g.field.inverse(zi, z2);
    g.field.mul(x, acc.x, zi);
    g.field.fromMont(x, x);
    if (compare(x, order.m, k) >= 0)
        subtract(x, order.m, k);
    return compare(x, rr, k) == 0;
}

} // namespace Kapala::Crypto
