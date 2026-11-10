#pragma once

#include "boot.h"

namespace Rudracore::Display {

// Draws the boot splash and routes kernel logs to an on-screen console.
// Does nothing (serial-only logging) when there's no usable framebuffer.
void init(Boot::Info const& info);

// Repaints the whole screen as the panic screen and points the console at it.
void showPanicScreen(char const* title);

// Updates the status bar at the bottom of the boot screen.
void drawStatus(u64 uptimeMs, u64 tasks, u64 freeBytes, u64 heapBytes);

// A user program (the display server) took the screen: stop drawing on it.
// Logs keep going to serial. Only a panic draws on the screen again.
void handOverToUserspace();

} // namespace Rudracore::Display
