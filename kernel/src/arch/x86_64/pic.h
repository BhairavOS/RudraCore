#pragma once

#include "types.h"

namespace Rudracore::Arch::Pic {

// The legacy 8259 PICs deliver IRQs 0-7 on vectors 8-15 by default, which
// collide with CPU exceptions. They are moved to 32-47 and fully masked;
// step 3 unmasks the timer.
constexpr u8 IRQ_BASE = 32;

void init();

void unmask(u8 irq);

void endOfInterrupt(u8 irq);

// IRQ 7 and 15 can fire spuriously; those must not be acknowledged.
bool isSpurious(u8 irq);

} // namespace Rudracore::Arch::Pic
