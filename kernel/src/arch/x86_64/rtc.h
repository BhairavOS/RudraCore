#pragma once

#include "types.h"

namespace Rudracore::Arch::Rtc {

// Reads the CMOS real-time clock: seconds since 1970-01-01 (the firmware
// keeps it in UTC on most systems, and QEMU does by default). Assumes the
// 21st century.
u64 unixTime();

} // namespace Rudracore::Arch::Rtc
