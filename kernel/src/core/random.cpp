#include "core/random.h"

namespace Rudracore::Random {

namespace {

u32 key[8];      // the generator's secret state
u32 pool[8];     // timings collected since the last use
u32 poolIndex = 0;
u64 counter = 0;
bool hardware = false;

u64 rdtsc() {
    u32 lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (u64{hi} << 32) | lo;
}

bool rdrand(u64& value) {
    if (not hardware)
        return false;
    for (int tries = 0; tries < 10; tries++) {
        unsigned char ok;
        asm volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ok));
        if (ok)
            return true;
    }
    return false;
}

u32 rotl(u32 v, int n) { return (v << n) | (v >> (32 - n)); }

void quarter(u32& a, u32& b, u32& c, u32& d) {
    a += b; d ^= a; d = rotl(d, 16);
    c += d; b ^= c; b = rotl(b, 12);
    a += b; d ^= a; d = rotl(d, 8);
    c += d; b ^= c; b = rotl(b, 7);
}

// One ChaCha20 block (RFC 8439) of the current key.
void block(u32 out[16], u64 n) {
    u32 s[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
                 key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7],
                 static_cast<u32>(n), static_cast<u32>(n >> 32), 0x42484149 /* "BHAI" */, 0x52415600};
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
        out[i] = x[i] + s[i];
}

// Folds the pool (and fresh hardware randomness) into a new key.
void rekey() {
    for (int i = 0; i < 8; i++) {
        key[i] ^= pool[i];
        pool[i] = 0;
    }
    u64 hw;
    for (int i = 0; i < 4; i++) {
        if (rdrand(hw)) {
            key[2 * i] ^= static_cast<u32>(hw);
            key[2 * i + 1] ^= static_cast<u32>(hw >> 32);
        }
    }
    u64 t = rdtsc();
    key[0] ^= static_cast<u32>(t);
    key[1] ^= static_cast<u32>(t >> 32);
    u32 next[16];
    block(next, counter++);
    for (int i = 0; i < 8; i++)
        key[i] = next[i]; // the old key can't be recovered from the new one
}

} // namespace

void init(u64 seed) {
    u32 a, b, c, d;
    asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    hardware = c & (1u << 30); // RDRAND
    key[0] = static_cast<u32>(seed);
    key[1] = static_cast<u32>(seed >> 32);
    // The cycle counter's low bits wobble from run to run: sample it many
    // times around work whose length varies.
    for (int i = 0; i < 64; i++) {
        u64 t = rdtsc();
        pool[i % 8] = rotl(pool[i % 8], 7) ^ static_cast<u32>(t) ^ static_cast<u32>(t >> 29);
        for (int spin = 0; spin < (static_cast<int>(t) & 63); spin++)
            asm volatile("pause");
    }
    rekey();
}

void stir() {
    u64 t = rdtsc();
    u32& slot = pool[poolIndex++ % 8];
    slot = rotl(slot, 5) ^ static_cast<u32>(t);
}

void fill(u8* out, usize length) {
    rekey();
    u32 words[16];
    for (usize done = 0; done < length;) {
        block(words, counter++);
        auto const* bytes = reinterpret_cast<u8 const*>(words);
        for (usize i = 0; i < 64 and done < length; i++)
            out[done++] = bytes[i];
    }
    rekey(); // forget the key that made this output
}

bool hasHardwareSource() { return hardware; }

} // namespace Rudracore::Random
