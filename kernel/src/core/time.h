#pragma once

#include "types.h"

namespace Rudracore::Time {

constexpr u32 TICKS_PER_SECOND = 1000;

// Starts the system timer (IRQ 0). Interrupts must be enabled separately.
void init();

// Called from the timer interrupt.
void tick();

// Milliseconds since the timer started.
u64 now();

// Seconds since midnight (UTC), from the hardware clock read at boot.
u64 timeOfDay();

} // namespace Rudracore::Time
