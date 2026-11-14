#include "core/random.h"

#include "log.h"

namespace Rudracore::Random {

namespace {

u32 key[8];      // the generator's secret state
u32 pool[8];     // timings collected since the last use
u32 poolIndex = 0;
u64 counter = 0;
bool hardware = false;     // RDRAND present and passing its tests
bool jitterHealthy = false; // the cycle-counter samples passed their start-up tests
bool kat = false;           // ChaCha20 gave the RFC 8439 answer
u64 lastHardware = 0;
u32 hardwareRepeats = 0;
Health status{};

u64 rdtsc() {
    u32 lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (u64{hi} << 32) | lo;
}

// Continuous test (SP 800-90B 4.4.1 style): a value equal to the last one is
// a failure, and after three the source is no longer used. (A healthy
// 64-bit generator repeats with probability 2^-64; some CPUs' RDRAND gets
// stuck, AMD's famously at all ones.)
bool rdrand(u64& value) {
    if (not hardware)
        return false;
    for (int tries = 0; tries < 10; tries++) {
        unsigned char ok;
        asm volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ok));
        if (not ok)
            continue;
        if (value == lastHardware or value == 0 or value == ~0ull) {
            if (++hardwareRepeats >= 3) {
                hardware = false;
                status.hardwareFailed = true;
                log("[rand] RDRAND repeats itself: no longer used");
                return false;
            }
            continue;
        }
        lastHardware = value;
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

// One ChaCha20 block (RFC 8439): words 12-13 are the 64-bit block number,
// 14-15 the rest of the nonce.
void chacha(u32 out[16], u32 const k[8], u64 n, u32 nonce14, u32 nonce15) {
    u32 s[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574, k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7],
                 static_cast<u32>(n), static_cast<u32>(n >> 32), nonce14, nonce15};
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

void block(u32 out[16], u64 n) { chacha(out, key, n, 0x42484149 /* "BHAI" */, 0x52415600); }

// Known-answer test: RFC 8439 section 2.3.2 (key 00..1f, counter 1,
// nonce 00 00 00 09 00 00 00 4a 00 00 00 00).
bool knownAnswer() {
    u32 k[8];
    for (u32 i = 0; i < 8; i++)
        k[i] = (4 * i) | (4 * i + 1) << 8 | (4 * i + 2) << 16 | (4 * i + 3) << 24;
    static constexpr u32 EXPECTED[16] = {0xe4e7f110, 0x15593bd1, 0x1fdd0f50, 0xc47120a3, 0xc7f4d1c7, 0x0368c033,
                                         0x9aaa2204, 0x4e6cd4c3, 0x466482d2, 0x09aa9f07, 0x05d7c214, 0xa2028bd9,
                                         0xd19c12b5, 0xb94e16de, 0xe883d0cb, 0x4e3c50a2};
    u32 out[16];
    chacha(out, k, 1 | u64{0x09000000} << 32, 0x4a000000, 0);
    for (int i = 0; i < 16; i++) {
        if (out[i] != EXPECTED[i])
            return false;
    }
    return true;
}

// Start-up tests of the cycle-counter source (SP 800-90B 4.4), on the low
// byte of the differences between samples taken around variable work,
// assuming (conservatively) 1 bit of entropy per sample:
//   repetition count: no value 21 times in a row (C = 1 + 20/H)
//   adaptive proportion: no value more than 410 times in 512 (H = 1)
bool jitterTests(u32 samples[], u32 count) {
    u32 run = 1;
    for (u32 i = 1; i < count; i++) {
        run = samples[i] == samples[i - 1] ? run + 1 : 1;
        if (run >= 21)
            return false;
    }
    for (u32 start = 0; start + 512 <= count; start += 512) {
        u32 same = 0;
        for (u32 i = start; i < start + 512; i++)
            same += samples[i] == samples[start];
        if (same > 410)
            return false;
    }
    return true;
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
    kat = knownAnswer();
    status.knownAnswer = kat;

    u32 a, b, c, d;
    asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    hardware = c & (1u << 30); // RDRAND
    status.hardwarePresent = hardware;
    if (hardware) {
        // Start-up test: 16 outputs, all different, none all-zero or all-one.
        u64 seen[16];
        for (int i = 0; i < 16 and hardware; i++) {
            if (not rdrand(seen[i]))
                hardware = false;
            for (int j = 0; j < i and hardware; j++)
                hardware = seen[j] != seen[i];
        }
        if (not hardware) {
            status.hardwareFailed = true;
            log("[rand] RDRAND failed its start-up test: not used");
        }
    }

    key[0] = static_cast<u32>(seed);
    key[1] = static_cast<u32>(seed >> 32);
    // The cycle counter's low bits wobble from run to run: sample it many
    // times around work whose length varies, test the samples, keep them all.
    static u32 samples[1024];
    u64 previous = rdtsc();
    for (u32 i = 0; i < 1024; i++) {
        for (int spin = 0; spin < (static_cast<int>(previous) & 63); spin++)
            asm volatile("pause");
        u64 t = rdtsc();
        samples[i] = static_cast<u32>(t - previous) & 0xFF;
        pool[i % 8] = rotl(pool[i % 8], 7) ^ static_cast<u32>(t) ^ static_cast<u32>(t >> 29);
        previous = t;
    }
    jitterHealthy = jitterTests(samples, 1024);
    status.jitterHealthy = jitterHealthy;
    rekey();
}

void stir() {
    u64 t = rdtsc();
    u32& slot = pool[poolIndex++ % 8];
    slot = rotl(slot, 5) ^ static_cast<u32>(t);
    status.stirs++;
}

bool ready() { return kat and (hardware or jitterHealthy); }

bool fill(u8* out, usize length) {
    if (not ready())
        return false; // fail closed: no output from a generator that can't be trusted
    rekey();
    u32 words[16];
    for (usize done = 0; done < length;) {
        block(words, counter++);
        auto const* bytes = reinterpret_cast<u8 const*>(words);
        for (usize i = 0; i < 64 and done < length; i++)
            out[done++] = bytes[i];
    }
    for (u32& w : words)
        *reinterpret_cast<u32 volatile*>(&w) = 0;
    rekey(); // forget the key that made this output
    return true;
}

bool hasHardwareSource() { return hardware; }

Health health() {
    status.hardwareInUse = hardware;
    status.ready = ready();
    return status;
}

} // namespace Rudracore::Random
