#pragma once

namespace Rudracore::Serial {

// COM1 at 38400 baud, 8N1. Used for kernel logs.
void init();

void write(char c);

} // namespace Rudracore::Serial
