#pragma once

#include "types.h"

namespace Rudracore {

// Stops the kernel with a message on the panic screen and serial log.
[[noreturn]] void panic(char const* message);

namespace Panic {

// Building blocks for panic reports with extra details (see idt.cpp).
// begin() disables interrupts and shows the panic screen; a panic raised
// while already panicking halts immediately instead of looping.
void begin(char const* title);

// Walks the frame-pointer chain starting at `rbp`.
void backtrace(u64 rbp);

[[noreturn]] void end();

} // namespace Panic

} // namespace Rudracore
