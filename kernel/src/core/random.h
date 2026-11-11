#pragma once

#include "types.h"

// Random numbers for programs (keys for encryption, TLS): a ChaCha20-based
// generator whose key is stirred with every bit of unpredictability the
// machine offers: the CPU's RDSEED/RDRAND instructions when it has them,
// and the cycle counter's exact value at each timer and device interrupt.
namespace Rudracore::Random {

// Seeds the generator (the hardware clock, the cycle counter, RDSEED/RDRAND).
void init(u64 seed);

// Called from interrupt handlers: mixes in the cycle counter's timing.
void stir();

// Fills `out` with random bytes.
void fill(u8* out, usize length);

// Whether the CPU has a hardware random number generator.
bool hasHardwareSource();

} // namespace Rudracore::Random
