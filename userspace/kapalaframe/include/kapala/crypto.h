#pragma once

// Kapalaframe: cryptography for TLS 1.3.
//
//   SHA-256, HMAC-SHA256 and HKDF          (FIPS 180-4, RFC 2104, RFC 5869)
//   X25519 key agreement                   (RFC 7748)
//   ChaCha20-Poly1305                      (RFC 8439)
//   AES-128-GCM                            (FIPS 197, NIST SP 800-38D)
//
// Written for clarity first. X25519 and ChaCha20-Poly1305 run in constant
// time; the AES here uses table lookups, which can leak timing to an
// attacker on the same machine.

namespace Kapala::Crypto {

using u8 = unsigned char;
using u32 = unsigned;
using u64 = unsigned long;

// MARK: Hashing

class Sha256 {
public:
    static constexpr u64 SIZE = 32;

    Sha256() { reset(); }
    void reset();
    void update(void const* data, u64 length);
    void finish(u8 out[32]); // the object can't be updated afterwards

    // The hash of everything so far, without finishing this object.
    void peek(u8 out[32]) const {
        Sha256 copy = *this;
        copy.finish(out);
    }

    static void hash(void const* data, u64 length, u8 out[32]) {
        Sha256 h;
        h.update(data, length);
        h.finish(out);
    }

private:
    void block(u8 const* p);

    u32 _state[8];
    u8 _buffer[64];
    u64 _length;
};

void hmacSha256(void const* key, u64 keyLength, void const* data, u64 length, u8 out[32]);

void hkdfExtract(void const* salt, u64 saltLength, void const* ikm, u64 ikmLength, u8 prk[32]);
void hkdfExpand(u8 const prk[32], void const* info, u64 infoLength, u8* out, u64 length);

// MARK: Key agreement

// out = scalar * point on Curve25519. With the base point (9) this makes a
// public key from a private one.
void x25519(u8 out[32], u8 const scalar[32], u8 const point[32]);
void x25519Base(u8 out[32], u8 const scalar[32]);

// MARK: Authenticated encryption (AEAD), 12-byte nonces, 16-byte tags

// Encrypts `length` bytes into out (which gets length + 16 bytes).
void chachaPolySeal(u8 const key[32], u8 const nonce[12], u8 const* aad, u64 aadLength, u8 const* plain,
                    u64 length, u8* out);
// Decrypts `length` bytes (the tag follows them); false if they were altered.
bool chachaPolyOpen(u8 const key[32], u8 const nonce[12], u8 const* aad, u64 aadLength, u8 const* cipher,
                    u64 length, u8* out);

void aesGcmSeal(u8 const key[16], u8 const nonce[12], u8 const* aad, u64 aadLength, u8 const* plain, u64 length,
                u8* out);
bool aesGcmOpen(u8 const key[16], u8 const nonce[12], u8 const* aad, u64 aadLength, u8 const* cipher, u64 length,
                u8* out);

// Compares without leaking where they differ.
bool equal(void const* a, void const* b, u64 length);

// Overwrites secrets in a way the compiler won't remove.
void wipe(void* data, u64 length);

} // namespace Kapala::Crypto
