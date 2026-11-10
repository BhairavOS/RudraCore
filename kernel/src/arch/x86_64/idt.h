#pragma once

#include "types.h"

namespace Rudracore::Arch::Idt {

constexpr usize STUB_SIZE = 16;

// Software interrupt kernel tasks use to give up the CPU (see Sched).
constexpr u8 YIELD_VECTOR = 0x81;

void init();

char const* exceptionName(u64 vector);

} // namespace Rudracore::Arch::Idt
