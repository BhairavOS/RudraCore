#pragma once

#include "types.h"

namespace Rudracore::Arch::Rtc {

// Reads the CMOS real-time clock: seconds since midnight (the firmware
// keeps it in UTC on most systems, and QEMU does by default).
u64 secondsOfDay();

} // namespace Rudracore::Arch::Rtc
