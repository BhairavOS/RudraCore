#pragma once

#include "types.h"

// Random numbers for programs (keys for encryption, TLS): a ChaCha20-based
// generator whose key is stirred with every bit of unpredictability the
// machine offers: the CPU's RDSEED/RDRAND instructions when it has them,
// and the cycle counter's exact value at each timer and device interrupt.
namespace Rudracore::Random {

// Seeds the generator (the hardware clock, the cycle counter, RDRAND) and
// runs the health tests: a ChaCha20 known-answer test (RFC 8439), RDRAND's
// start-up test (16 different outputs), and SP 800-90B-style repetition
// count and adaptive proportion tests on 1024 cycle-counter samples. RDRAND
// is also tested continuously (repeated outputs turn it off).
void init(u64 seed);

// Called from interrupt handlers: mixes in the cycle counter's timing.
void stir();

// Fills `out` with random bytes. False (and nothing written) unless the
// generator passed its known-answer test and has at least one healthy
// entropy source: callers must fail closed.
bool fill(u8* out, usize length);

// Whether RDRAND is present and passing its tests.
bool hasHardwareSource();

struct Health {
    bool knownAnswer;     // ChaCha20 matches RFC 8439
    bool hardwarePresent; // the CPU has RDRAND
    bool hardwareFailed;  // ...and it failed a test
    bool hardwareInUse;
    bool jitterHealthy;   // the cycle-counter samples passed
    bool ready;           // fill() gives output
    u64 stirs;            // interrupt timings mixed in so far
};
Health health();

} // namespace Rudracore::Random
