#include <kapala/crypto.h>

extern "C" {
void* memcpy(void* dest, void const* src, decltype(sizeof(0)) n);
void* memset(void* dest, int value, decltype(sizeof(0)) n);
}

namespace Kapala::Crypto {

namespace {

u32 rotr(u32 v, int n) { return (v >> n) | (v << (32 - n)); }
u32 rotl(u32 v, int n) { return (v << n) | (v >> (32 - n)); }
u32 load32be(u8 const* p) { return u32{p[0]} << 24 | u32{p[1]} << 16 | u32{p[2]} << 8 | p[3]; }
u32 load32le(u8 const* p) { return u32{p[0]} | u32{p[1]} << 8 | u32{p[2]} << 16 | u32{p[3]} << 24; }
void store32be(u8* p, u32 v) {
    p[0] = static_cast<u8>(v >> 24);
    p[1] = static_cast<u8>(v >> 16);
    p[2] = static_cast<u8>(v >> 8);
    p[3] = static_cast<u8>(v);
}
void store32le(u8* p, u32 v) {
    for (int i = 0; i < 4; i++)
        p[i] = static_cast<u8>(v >> (8 * i));
}
void store64le(u8* p, u64 v) {
    for (int i = 0; i < 8; i++)
        p[i] = static_cast<u8>(v >> (8 * i));
}

constexpr u32 K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

} // namespace

// MARK: SHA-256

void Sha256::reset() {
    static constexpr u32 INIT[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    for (int i = 0; i < 8; i++)
        _state[i] = INIT[i];
    _length = 0;
}

void Sha256::block(u8 const* p) {
    u32 w[64];
    for (int i = 0; i < 16; i++)
        w[i] = load32be(p + 4 * i);
    for (int i = 16; i < 64; i++) {
        u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = _state[0], b = _state[1], c = _state[2], d = _state[3];
    u32 e = _state[4], f = _state[5], g = _state[6], h = _state[7];
    for (int i = 0; i < 64; i++) {
        u32 t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        u32 t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    _state[0] += a;
    _state[1] += b;
    _state[2] += c;
    _state[3] += d;
    _state[4] += e;
    _state[5] += f;
    _state[6] += g;
    _state[7] += h;
}

void Sha256::update(void const* data, u64 length) {
    auto const* p = static_cast<u8 const*>(data);
    u64 used = _length % 64;
    _length += length;
    if (used) {
        u64 n = 64 - used < length ? 64 - used : length;
        memcpy(_buffer + used, p, n);
        p += n;
        length -= n;
        if (used + n < 64)
            return;
        block(_buffer);
    }
    while (length >= 64) {
        block(p);
        p += 64;
        length -= 64;
    }
    memcpy(_buffer, p, length);
}

void Sha256::finish(u8 out[32]) {
    u64 bits = _length * 8;
    u8 pad = 0x80;
    update(&pad, 1);
    u8 zero = 0;
    while (_length % 64 != 56)
        update(&zero, 1);
    u8 length[8];
    for (int i = 0; i < 8; i++)
        length[i] = static_cast<u8>(bits >> (56 - 8 * i));
    update(length, 8);
    for (int i = 0; i < 8; i++)
        store32be(out + 4 * i, _state[i]);
}

void hmacSha256(void const* key, u64 keyLength, void const* data, u64 length, u8 out[32]) {
    u8 k[64] = {};
    if (keyLength > 64)
        Sha256::hash(key, keyLength, k);
    else
        memcpy(k, key, keyLength);
    u8 pad[64];
    for (int i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x36;
    Sha256 inner;
    inner.update(pad, 64);
    inner.update(data, length);
    u8 innerHash[32];
    inner.finish(innerHash);
    for (int i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x5c;
    Sha256 outer;
    outer.update(pad, 64);
    outer.update(innerHash, 32);
    outer.finish(out);
    wipe(k, sizeof(k));
}

void hkdfExtract(void const* salt, u64 saltLength, void const* ikm, u64 ikmLength, u8 prk[32]) {
    u8 zeros[32] = {};
    if (saltLength == 0) {
        salt = zeros;
        saltLength = 32;
    }
    hmacSha256(salt, saltLength, ikm, ikmLength, prk);
}

void hkdfExpand(u8 const prk[32], void const* info, u64 infoLength, u8* out, u64 length) {
    u8 t[32];
    u64 tLength = 0;
    u8 buffer[32 + 256 + 1];
    for (u8 counter = 1; length; counter++) {
        memcpy(buffer, t, tLength);
        memcpy(buffer + tLength, info, infoLength);
        buffer[tLength + infoLength] = counter;
        hmacSha256(prk, 32, buffer, tLength + infoLength + 1, t);
        tLength = 32;
        u64 n = length < 32 ? length : 32;
        memcpy(out, t, n);
        out += n;
        length -= n;
    }
}

// MARK: SHA-512 / SHA-384

namespace {

constexpr u64 K512[80] = {
    0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f, 0xe9b5dba58189dbbc, 0x3956c25bf348b538,
    0x59f111f1b605d019, 0x923f82a4af194f9b, 0xab1c5ed5da6d8118, 0xd807aa98a3030242, 0x12835b0145706fbe,
    0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2, 0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235,
    0xc19bf174cf692694, 0xe49b69c19ef14ad2, 0xefbe4786384f25e3, 0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65,
    0x2de92c6f592b0275, 0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5, 0x983e5152ee66dfab,
    0xa831c66d2db43210, 0xb00327c898fb213f, 0xbf597fc7beef0ee4, 0xc6e00bf33da88fc2, 0xd5a79147930aa725,
    0x06ca6351e003826f, 0x142929670a0e6e70, 0x27b70a8546d22ffc, 0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed,
    0x53380d139d95b3df, 0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6, 0x92722c851482353b,
    0xa2bfe8a14cf10364, 0xa81a664bbc423001, 0xc24b8b70d0f89791, 0xc76c51a30654be30, 0xd192e819d6ef5218,
    0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8, 0x19a4c116b8d2d0c8, 0x1e376c085141ab53,
    0x2748774cdf8eeb99, 0x34b0bcb5e19b48a8, 0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb, 0x5b9cca4f7763e373,
    0x682e6ff3d6b2b8a3, 0x748f82ee5defb2fc, 0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec,
    0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915, 0xc67178f2e372532b, 0xca273eceea26619c,
    0xd186b8c721c0c207, 0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178, 0x06f067aa72176fba, 0x0a637dc5a2c898a6,
    0x113f9804bef90dae, 0x1b710b35131c471b, 0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc,
    0x431d67c49c100d4c, 0x4cc5d4becb3e42b6, 0x597f299cfc657e2a, 0x5fcb6fab3ad6faec, 0x6c44198c4a475817,
};

u64 rotr64(u64 v, int n) { return (v >> n) | (v << (64 - n)); }

u64 load64be(u8 const* p) {
    u64 v = 0;
    for (int i = 0; i < 8; i++)
        v = v << 8 | p[i];
    return v;
}

} // namespace

void Sha512::reset(bool sha384) {
    static constexpr u64 H512[8] = {0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1,
                                    0x510e527fade682d1, 0x9b05688c2b3e6c1f, 0x1f83d9abfb41bd6b, 0x5be0cd19137e2179};
    static constexpr u64 H384[8] = {0xcbbb9d5dc1059ed8, 0x629a292a367cd507, 0x9159015a3070dd17, 0x152fecd8f70e5939,
                                    0x67332667ffc00b31, 0x8eb44a8768581511, 0xdb0c2e0d64f98fa7, 0x47b5481dbefa4fa4};
    for (int i = 0; i < 8; i++)
        _state[i] = sha384 ? H384[i] : H512[i];
    _length = 0;
    _384 = sha384;
}

void Sha512::block(u8 const* p) {
    u64 w[80];
    for (int i = 0; i < 16; i++)
        w[i] = load64be(p + 8 * i);
    for (int i = 16; i < 80; i++) {
        u64 s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        u64 s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u64 a = _state[0], b = _state[1], c = _state[2], d = _state[3];
    u64 e = _state[4], f = _state[5], g = _state[6], h = _state[7];
    for (int i = 0; i < 80; i++) {
        u64 t1 = h + (rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41)) + ((e & f) ^ (~e & g)) + K512[i] + w[i];
        u64 t2 = (rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    _state[0] += a;
    _state[1] += b;
    _state[2] += c;
    _state[3] += d;
    _state[4] += e;
    _state[5] += f;
    _state[6] += g;
    _state[7] += h;
}

void Sha512::update(void const* data, u64 length) {
    auto const* p = static_cast<u8 const*>(data);
    while (length) {
        u64 used = _length % 128;
        u64 n = 128 - used < length ? 128 - used : length;
        memcpy(_buffer + used, p, n);
        _length += n;
        p += n;
        length -= n;
        if (_length % 128 == 0)
            block(_buffer);
    }
}

void Sha512::finish(u8* out) {
    u64 bits = _length * 8;
    u8 pad = 0x80;
    update(&pad, 1);
    u8 zero = 0;
    while (_length % 128 != 112)
        update(&zero, 1);
    u8 size[16] = {};
    for (int i = 0; i < 8; i++)
        size[15 - i] = static_cast<u8>(bits >> (8 * i));
    update(size, 16);
    int words = _384 ? 6 : 8;
    for (int i = 0; i < words; i++)
        for (int j = 0; j < 8; j++)
            out[8 * i + j] = static_cast<u8>(_state[i] >> (56 - 8 * j));
}

// MARK: X25519 (after TweetNaCl, public domain)

namespace {

using i64 = long long;
using Field = i64[16];

constexpr Field F121665 = {0xDB41, 1};

void carry(Field o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (i64{1} << 16);
        i64 c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * 65536;
    }
}

void select(Field p, Field q, int b) {
    i64 c = ~(b - 1);
    for (int i = 0; i < 16; i++) {
        i64 t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

void pack(u8* o, Field const n) {
    Field m, t;
    for (int i = 0; i < 16; i++)
        t[i] = n[i];
    carry(t);
    carry(t);
    carry(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = static_cast<int>((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        select(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2 * i] = static_cast<u8>(t[i] & 0xff);
        o[2 * i + 1] = static_cast<u8>(t[i] >> 8);
    }
}

void unpack(Field o, u8 const* n) {
    for (int i = 0; i < 16; i++)
        o[i] = n[2 * i] + (i64{n[2 * i + 1]} << 8);
    o[15] &= 0x7fff;
}

void add(Field o, Field const a, Field const b) {
    for (int i = 0; i < 16; i++)
        o[i] = a[i] + b[i];
}

void sub(Field o, Field const a, Field const b) {
    for (int i = 0; i < 16; i++)
        o[i] = a[i] - b[i];
}

void mul(Field o, Field const a, Field const b) {
    i64 t[31] = {};
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++)
        t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++)
        o[i] = t[i];
    carry(o);
    carry(o);
}

void square(Field o, Field const a) { mul(o, a, a); }

void invert(Field o, Field const in) {
    Field c;
    for (int i = 0; i < 16; i++)
        c[i] = in[i];
    for (int a = 253; a >= 0; a--) {
        square(c, c);
        if (a != 2 and a != 4)
            mul(c, c, in);
    }
    for (int i = 0; i < 16; i++)
        o[i] = c[i];
}

} // namespace

void x25519(u8 out[32], u8 const scalar[32], u8 const point[32]) {
    u8 z[32];
    for (int i = 0; i < 32; i++)
        z[i] = scalar[i];
    z[31] = static_cast<u8>((scalar[31] & 127) | 64);
    z[0] &= 248;

    Field x, a, b, c, d, e, f;
    unpack(x, point);
    for (int i = 0; i < 16; i++) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        select(a, b, r);
        select(c, d, r);
        add(e, a, c);
        sub(a, a, c);
        add(c, b, d);
        sub(b, b, d);
        square(d, e);
        square(f, a);
        mul(a, c, a);
        mul(c, b, e);
        add(e, a, c);
        sub(a, a, c);
        square(b, a);
        sub(c, d, f);
        mul(a, c, F121665);
        add(a, a, d);
        mul(c, c, a);
        mul(a, d, f);
        mul(d, b, x);
        square(b, e);
        select(a, b, r);
        select(c, d, r);
    }
    invert(c, c);
    mul(a, a, c);
    pack(out, a);
    wipe(z, sizeof(z));
}

void x25519Base(u8 out[32], u8 const scalar[32]) {
    static constexpr u8 BASE[32] = {9};
    x25519(out, scalar, BASE);
}

// MARK: Ed25519 verification (after TweetNaCl, public domain)

namespace {

constexpr Field ED_D = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
                        0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
constexpr Field ED_D2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
                         0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
constexpr Field ED_X = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
                        0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
constexpr Field ED_Y = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
                        0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
constexpr Field ED_I = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
                        0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};
constexpr u64 ED_L[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                          0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0x10};

using Point = Field[4]; // extended coordinates X, Y, Z, T

void copyField(Field o, Field const a) {
    for (int i = 0; i < 16; i++)
        o[i] = a[i];
}

void setSmall(Field o, i64 v) {
    for (int i = 0; i < 16; i++)
        o[i] = 0;
    o[0] = v;
}

bool fieldsDiffer(Field const a, Field const b) {
    u8 x[32], y[32];
    pack(x, a);
    pack(y, b);
    return not equal(x, y, 32);
}

int parity(Field const a) {
    u8 d[32];
    pack(d, a);
    return d[0] & 1;
}

// a ^ ((p - 5) / 8), for the square root in decompression.
void pow2523(Field o, Field const in) {
    Field c;
    copyField(c, in);
    for (int a = 250; a >= 0; a--) {
        square(c, c);
        if (a != 1)
            mul(c, c, in);
    }
    copyField(o, c);
}

void pointAdd(Point p, Point const q) {
    Field a, b, c, d, t, e, f, g, h;
    sub(a, p[1], p[0]);
    sub(t, q[1], q[0]);
    mul(a, a, t);
    add(b, p[0], p[1]);
    add(t, q[0], q[1]);
    mul(b, b, t);
    mul(c, p[3], q[3]);
    mul(c, c, ED_D2);
    mul(d, p[2], q[2]);
    add(d, d, d);
    sub(e, b, a);
    sub(f, d, c);
    add(g, d, c);
    add(h, b, a);
    mul(p[0], e, f);
    mul(p[1], h, g);
    mul(p[2], g, f);
    mul(p[3], e, h);
}

void pointSwap(Point p, Point q, int b) {
    for (int i = 0; i < 4; i++)
        select(p[i], q[i], b);
}

void pointPack(u8 out[32], Point const p) {
    Field zi, tx, ty;
    invert(zi, p[2]);
    mul(tx, p[0], zi);
    mul(ty, p[1], zi);
    pack(out, ty);
    out[31] ^= static_cast<u8>(parity(tx) << 7);
}

void scalarMultiply(Point p, Point q, u8 const* s) {
    setSmall(p[0], 0);
    setSmall(p[1], 1);
    setSmall(p[2], 1);
    setSmall(p[3], 0);
    for (int i = 255; i >= 0; --i) {
        int b = (s[i / 8] >> (i & 7)) & 1;
        pointSwap(p, q, b);
        pointAdd(q, p);
        pointAdd(p, p);
        pointSwap(p, q, b);
    }
}

void scalarBase(Point p, u8 const* s) {
    Point q;
    copyField(q[0], ED_X);
    copyField(q[1], ED_Y);
    setSmall(q[2], 1);
    mul(q[3], ED_X, ED_Y);
    scalarMultiply(p, q, s);
}

// r = x mod L (x: 64 limbs of one byte each).
void modL(u8* r, i64 x[64]) {
    for (int i = 63; i >= 32; --i) {
        i64 c = 0;
        int j;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += c - 16 * x[i] * static_cast<i64>(ED_L[j - (i - 32)]);
            c = (x[j] + 128) >> 8;
            x[j] -= c * 256;
        }
        x[j] += c;
        x[i] = 0;
    }
    i64 c = 0;
    for (int j = 0; j < 32; j++) {
        x[j] += c - (x[31] >> 4) * static_cast<i64>(ED_L[j]);
        c = x[j] >> 8;
        x[j] &= 255;
    }
    for (int j = 0; j < 32; j++)
        x[j] -= c * static_cast<i64>(ED_L[j]);
    for (int i = 0; i < 32; i++) {
        x[i + 1] += x[i] >> 8;
        r[i] = static_cast<u8>(x[i] & 255);
    }
}

// Decompresses a public key into -A (negated, as verification needs).
bool unpackNegated(Point r, u8 const p[32]) {
    Field t, chk, num, den, den2, den4, den6;
    setSmall(r[2], 1);
    unpack(r[1], p);
    square(num, r[1]);
    mul(den, num, ED_D);
    sub(num, num, r[2]);
    add(den, r[2], den);
    square(den2, den);
    square(den4, den2);
    mul(den6, den4, den2);
    mul(t, den6, num);
    mul(t, t, den);
    pow2523(t, t);
    mul(t, t, num);
    mul(t, t, den);
    mul(t, t, den);
    mul(r[0], t, den);
    square(chk, r[0]);
    mul(chk, chk, den);
    if (fieldsDiffer(chk, num))
        mul(r[0], r[0], ED_I);
    square(chk, r[0]);
    mul(chk, chk, den);
    if (fieldsDiffer(chk, num))
        return false; // not a point on the curve
    if (parity(r[0]) == (p[31] >> 7)) {
        Field zero = {};
        sub(r[0], zero, r[0]);
    }
    mul(r[3], r[0], r[1]);
    return true;
}

} // namespace

bool ed25519Verify(u8 const signature[64], void const* message, u64 length, u8 const publicKey[32]) {
    // S must be below L (RFC 8032 5.1.7): reject the malleable forms.
    for (int i = 31; i >= 0; i--) {
        if (signature[32 + i] < ED_L[i])
            break;
        if (signature[32 + i] > ED_L[i] or i == 0)
            return false;
    }
    Point a;
    if (not unpackNegated(a, publicKey))
        return false;

    Sha512 h;
    h.update(signature, 32);
    h.update(publicKey, 32);
    h.update(message, length);
    u8 k[64];
    h.finish(k);
    i64 wide[64];
    for (int i = 0; i < 64; i++)
        wide[i] = k[i];
    u8 reduced[32];
    modL(reduced, wide);

    Point p, q;
    scalarMultiply(p, a, reduced); // k * (-A)
    scalarBase(q, signature + 32); // S * B
    pointAdd(p, q);
    u8 r[32];
    pointPack(r, p);
    return equal(r, signature, 32); // R == S*B - k*A
}

// MARK: ChaCha20 and Poly1305

namespace {

void quarter(u32& a, u32& b, u32& c, u32& d) {
    a += b; d ^= a; d = rotl(d, 16);
    c += d; b ^= c; b = rotl(b, 12);
    a += b; d ^= a; d = rotl(d, 8);
    c += d; b ^= c; b = rotl(b, 7);
}

void chachaBlock(u8 const key[32], u32 counter, u8 const nonce[12], u8 out[64]) {
    u32 s[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; i++)
        s[4 + i] = load32le(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++)
        s[13 + i] = load32le(nonce + 4 * i);
    u32 x[16];
    for (int i = 0; i < 16; i++)
        x[i] = s[i];
    for (int round = 0; round < 10; round++) {
        quarter(x[0], x[4], x[8], x[12]);
        quarter(x[1], x[5], x[9], x[13]);
        quarter(x[2], x[6], x[10], x[14]);
        quarter(x[3], x[7], x[11], x[15]);
        quarter(x[0], x[5], x[10], x[15]);
        quarter(x[1], x[6], x[11], x[12]);
        quarter(x[2], x[7], x[8], x[13]);
        quarter(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++)
        store32le(out + 4 * i, x[i] + s[i]);
}

void chachaXor(u8 const key[32], u32 counter, u8 const nonce[12], u8 const* in, u64 length, u8* out) {
    u8 stream[64];
    for (u64 done = 0; done < length; done += 64) {
        chachaBlock(key, counter++, nonce, stream);
        u64 n = length - done < 64 ? length - done : 64;
        for (u64 i = 0; i < n; i++)
            out[done + i] = in[done + i] ^ stream[i];
    }
    wipe(stream, sizeof(stream));
}

// Poly1305 (after poly1305-donna, public domain): 26-bit limbs.
class Poly1305 {
public:
    explicit Poly1305(u8 const key[32]) {
        _r[0] = load32le(key + 0) & 0x3ffffff;
        _r[1] = (load32le(key + 3) >> 2) & 0x3ffff03;
        _r[2] = (load32le(key + 6) >> 4) & 0x3ffc0ff;
        _r[3] = (load32le(key + 9) >> 6) & 0x3f03fff;
        _r[4] = (load32le(key + 12) >> 8) & 0x00fffff;
        for (int i = 0; i < 4; i++)
            _pad[i] = load32le(key + 16 + 4 * i);
    }

    // Whole 16-byte blocks; a shorter final block is padded with zeros
    // (which is what the AEAD construction wants).
    void update(u8 const* data, u64 length) {
        while (length) {
            u8 block[16] = {};
            u64 n = length < 16 ? length : 16;
            memcpy(block, data, n);
            process(block);
            data += n;
            length -= n;
        }
    }

    void finish(u8 out[16]) {
        u32 h0 = _h[0], h1 = _h[1], h2 = _h[2], h3 = _h[3], h4 = _h[4];
        u32 c = h1 >> 26; h1 &= 0x3ffffff;
        h2 += c; c = h2 >> 26; h2 &= 0x3ffffff;
        h3 += c; c = h3 >> 26; h3 &= 0x3ffffff;
        h4 += c; c = h4 >> 26; h4 &= 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
        h1 += c;

        u32 g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
        u32 g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
        u32 g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
        u32 g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
        u32 g4 = h4 + c - (1u << 26);

        u32 mask = (g4 >> 31) - 1; // all ones if h >= p (then use g)
        g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
        mask = ~mask;
        h0 = (h0 & mask) | g0;
        h1 = (h1 & mask) | g1;
        h2 = (h2 & mask) | g2;
        h3 = (h3 & mask) | g3;
        h4 = (h4 & mask) | g4;

        h0 = h0 | (h1 << 26);
        h1 = (h1 >> 6) | (h2 << 20);
        h2 = (h2 >> 12) | (h3 << 14);
        h3 = (h3 >> 18) | (h4 << 8);

        u64 f = u64{h0} + _pad[0];
        store32le(out, static_cast<u32>(f));
        f = u64{h1} + _pad[1] + (f >> 32);
        store32le(out + 4, static_cast<u32>(f));
        f = u64{h2} + _pad[2] + (f >> 32);
        store32le(out + 8, static_cast<u32>(f));
        f = u64{h3} + _pad[3] + (f >> 32);
        store32le(out + 12, static_cast<u32>(f));
    }

private:
    void process(u8 const* m) {
        u32 r0 = _r[0], r1 = _r[1], r2 = _r[2], r3 = _r[3], r4 = _r[4];
        u32 s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
        u32 h0 = _h[0], h1 = _h[1], h2 = _h[2], h3 = _h[3], h4 = _h[4];
        h0 += load32le(m + 0) & 0x3ffffff;
        h1 += (load32le(m + 3) >> 2) & 0x3ffffff;
        h2 += (load32le(m + 6) >> 4) & 0x3ffffff;
        h3 += (load32le(m + 9) >> 6) & 0x3ffffff;
        h4 += (load32le(m + 12) >> 8) | (1u << 24);

        u64 d0 = u64{h0} * r0 + u64{h1} * s4 + u64{h2} * s3 + u64{h3} * s2 + u64{h4} * s1;
        u64 d1 = u64{h0} * r1 + u64{h1} * r0 + u64{h2} * s4 + u64{h3} * s3 + u64{h4} * s2;
        u64 d2 = u64{h0} * r2 + u64{h1} * r1 + u64{h2} * r0 + u64{h3} * s4 + u64{h4} * s3;
        u64 d3 = u64{h0} * r3 + u64{h1} * r2 + u64{h2} * r1 + u64{h3} * r0 + u64{h4} * s4;
        u64 d4 = u64{h0} * r4 + u64{h1} * r3 + u64{h2} * r2 + u64{h3} * r1 + u64{h4} * r0;

        u32 c = static_cast<u32>(d0 >> 26); h0 = static_cast<u32>(d0) & 0x3ffffff;
        d1 += c; c = static_cast<u32>(d1 >> 26); h1 = static_cast<u32>(d1) & 0x3ffffff;
        d2 += c; c = static_cast<u32>(d2 >> 26); h2 = static_cast<u32>(d2) & 0x3ffffff;
        d3 += c; c = static_cast<u32>(d3 >> 26); h3 = static_cast<u32>(d3) & 0x3ffffff;
        d4 += c; c = static_cast<u32>(d4 >> 26); h4 = static_cast<u32>(d4) & 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
        h1 += c;
        _h[0] = h0; _h[1] = h1; _h[2] = h2; _h[3] = h3; _h[4] = h4;
    }

    u32 _r[5];
    u32 _h[5] = {};
    u32 _pad[4];
};

void chachaPolyTag(u8 const key[32], u8 const nonce[12], u8 const* aad, u64 aadLength, u8 const* cipher,
                   u64 length, u8 tag[16]) {
    u8 polyKey[64];
    chachaBlock(key, 0, nonce, polyKey);
    Poly1305 poly(polyKey);
    poly.update(aad, aadLength);
    poly.update(cipher, length);
    u8 lengths[16];
    store64le(lengths, aadLength);
    store64le(lengths + 8, length);
    poly.update(lengths, 16);
    poly.finish(tag);
    wipe(polyKey, sizeof(polyKey));
}

} // namespace

void chachaPolySeal(u8 const key[32], u8 const nonce[12], u8 const* aad, u64 aadLength, u8 const* plain,
                    u64 length, u8* out) {
    chachaXor(key, 1, nonce, plain, length, out);
    chachaPolyTag(key, nonce, aad, aadLength, out, length, out + length);
}

bool chachaPolyOpen(u8 const key[32], u8 const nonce[12], u8 const* aad, u64 aadLength, u8 const* cipher,
                    u64 length, u8* out) {
    u8 tag[16];
    chachaPolyTag(key, nonce, aad, aadLength, cipher, length, tag);
    if (not equal(tag, cipher + length, 16))
        return false;
    chachaXor(key, 1, nonce, cipher, length, out);
    return true;
}

// MARK: AES-128 and GCM

namespace {

constexpr u8 SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

u8 xtime(u8 x) { return static_cast<u8>((x << 1) ^ ((x >> 7) * 0x1b)); }

struct Aes128 {
    u8 roundKeys[176];

    explicit Aes128(u8 const key[16]) {
        memcpy(roundKeys, key, 16);
        u8 rcon = 1;
        for (int i = 16; i < 176; i += 4) {
            u8 t[4] = {roundKeys[i - 4], roundKeys[i - 3], roundKeys[i - 2], roundKeys[i - 1]};
            if (i % 16 == 0) {
                u8 first = t[0];
                t[0] = static_cast<u8>(SBOX[t[1]] ^ rcon);
                t[1] = SBOX[t[2]];
                t[2] = SBOX[t[3]];
                t[3] = SBOX[first];
                rcon = xtime(rcon);
            }
            for (int k = 0; k < 4; k++)
                roundKeys[i + k] = roundKeys[i - 16 + k] ^ t[k];
        }
    }

    ~Aes128() { wipe(roundKeys, sizeof(roundKeys)); }

    void encrypt(u8 const in[16], u8 out[16]) const {
        u8 s[16];
        for (int i = 0; i < 16; i++)
            s[i] = in[i] ^ roundKeys[i];
        for (int round = 1; round <= 10; round++) {
            // SubBytes and ShiftRows (column-major state)
            u8 t[16];
            for (int c = 0; c < 4; c++)
                for (int r = 0; r < 4; r++)
                    t[4 * c + r] = SBOX[s[4 * ((c + r) % 4) + r]];
            // MixColumns (not in the last round)
            if (round < 10) {
                for (int c = 0; c < 4; c++) {
                    u8* col = t + 4 * c;
                    u8 a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                    u8 all = a0 ^ a1 ^ a2 ^ a3;
                    col[0] = static_cast<u8>(a0 ^ all ^ xtime(a0 ^ a1));
                    col[1] = static_cast<u8>(a1 ^ all ^ xtime(a1 ^ a2));
                    col[2] = static_cast<u8>(a2 ^ all ^ xtime(a2 ^ a3));
                    col[3] = static_cast<u8>(a3 ^ all ^ xtime(a3 ^ a0));
                }
            }
            for (int i = 0; i < 16; i++)
                s[i] = t[i] ^ roundKeys[16 * round + i];
        }
        memcpy(out, s, 16);
    }
};

// Multiplication in GF(2^128) as GCM defines it (bit 0 is the top bit).
void gfMultiply(u8 x[16], u8 const h[16]) {
    u64 vh = 0, vl = 0, zh = 0, zl = 0;
    for (int i = 0; i < 8; i++) {
        vh = vh << 8 | h[i];
        vl = vl << 8 | h[8 + i];
    }
    for (int i = 0; i < 128; i++) {
        u64 bit = (x[i / 8] >> (7 - i % 8)) & 1;
        u64 mask = 0 - bit;
        zh ^= vh & mask;
        zl ^= vl & mask;
        u64 lsb = vl & 1;
        vl = (vl >> 1) | (vh << 63);
        vh = (vh >> 1) ^ ((0 - lsb) & 0xe100000000000000ul);
    }
    for (int i = 0; i < 8; i++) {
        x[i] = static_cast<u8>(zh >> (56 - 8 * i));
        x[8 + i] = static_cast<u8>(zl >> (56 - 8 * i));
    }
}

void ghash(u8 const h[16], u8 const* aad, u64 aadLength, u8 const* cipher, u64 length, u8 out[16]) {
    u8 y[16] = {};
    auto absorb = [&](u8 const* data, u64 n) {
        for (u64 done = 0; done < n; done += 16) {
            u64 k = n - done < 16 ? n - done : 16;
            for (u64 i = 0; i < k; i++)
                y[i] ^= data[done + i];
            gfMultiply(y, h);
        }
    };
    absorb(aad, aadLength);
    absorb(cipher, length);
    u8 lengths[16];
    u64 aadBits = aadLength * 8, bits = length * 8;
    for (int i = 0; i < 8; i++) {
        lengths[i] = static_cast<u8>(aadBits >> (56 - 8 * i));
        lengths[8 + i] = static_cast<u8>(bits >> (56 - 8 * i));
    }
    absorb(lengths, 16);
    memcpy(out, y, 16);
}

void gcmCtr(Aes128 const& aes, u8 const nonce[12], u8 const* in, u64 length, u8* out) {
    u8 counter[16];
    memcpy(counter, nonce, 12);
    u32 n = 2; // 1 is for the tag
    u8 stream[16];
    for (u64 done = 0; done < length; done += 16) {
        store32be(counter + 12, n++);
        aes.encrypt(counter, stream);
        u64 k = length - done < 16 ? length - done : 16;
        for (u64 i = 0; i < k; i++)
            out[done + i] = in[done + i] ^ stream[i];
    }
}

void gcmTag(Aes128 const& aes, u8 const nonce[12], u8 const* aad, u64 aadLength, u8 const* cipher, u64 length,
            u8 tag[16]) {
    u8 h[16] = {}, zero[16] = {};
    aes.encrypt(zero, h);
    u8 s[16];
    ghash(h, aad, aadLength, cipher, length, s);
    u8 j0[16];
    memcpy(j0, nonce, 12);
    store32be(j0 + 12, 1);
    u8 e[16];
    aes.encrypt(j0, e);
    for (int i = 0; i < 16; i++)
        tag[i] = e[i] ^ s[i];
}

} // namespace

void aesGcmSeal(u8 const key[16], u8 const nonce[12], u8 const* aad, u64 aadLength, u8 const* plain, u64 length,
                u8* out) {
    Aes128 aes(key);
    gcmCtr(aes, nonce, plain, length, out);
    gcmTag(aes, nonce, aad, aadLength, out, length, out + length);
}

bool aesGcmOpen(u8 const key[16], u8 const nonce[12], u8 const* aad, u64 aadLength, u8 const* cipher, u64 length,
                u8* out) {
    Aes128 aes(key);
    u8 tag[16];
    gcmTag(aes, nonce, aad, aadLength, cipher, length, tag);
    if (not equal(tag, cipher + length, 16))
        return false;
    gcmCtr(aes, nonce, cipher, length, out);
    return true;
}

// MARK: Helpers

bool equal(void const* a, void const* b, u64 length) {
    auto const* x = static_cast<u8 const*>(a);
    auto const* y = static_cast<u8 const*>(b);
    u8 diff = 0;
    for (u64 i = 0; i < length; i++)
        diff |= x[i] ^ y[i];
    return diff == 0;
}

void wipe(void* data, u64 length) {
    auto* p = static_cast<u8 volatile*>(data);
    for (u64 i = 0; i < length; i++)
        p[i] = 0;
}

} // namespace Kapala::Crypto
