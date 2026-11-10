#pragma once

#include "types.h"

namespace Rudracore::Arch::Idt {

constexpr usize STUB_SIZE = 16;

void init();

char const* exceptionName(u64 vector);

} // namespace Rudracore::Arch::Idt
