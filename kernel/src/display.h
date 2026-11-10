#pragma once

#include "boot.h"

namespace Rudracore::Display {

// Draws the boot splash and routes kernel logs to an on-screen console.
// Does nothing (serial-only logging) when there's no usable framebuffer.
void init(Boot::Info const& info);

// Repaints the whole screen as the panic screen and points the console at it.
void showPanicScreen(char const* title);

} // namespace Rudracore::Display
