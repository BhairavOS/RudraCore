#pragma once

#include "types.h"

namespace Rudracore::Arch::Pit {

// Programs the legacy PIT to fire IRQ 0 `hz` times per second.
void init(u32 hz);

} // namespace Rudracore::Arch::Pit
